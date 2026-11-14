# SPDX-License-Identifier: GPL-3.0-only
"""Checks that the firmware, the phone page and the mock bridge agree:

- every /status field the page reads is sent by the firmware, and the mock
  sends exactly the firmware's fields;
- every picture command (/cmd?c=X) and receiver key (/c5?k=X) on the page is
  one the firmware accepts, and the mock accepts the same ones;
- the console help lists every picture command.

  python tools/check_consistency.py        (also run by bridge/test/run_host_tests.py)
Reads the sources as text; needs nothing but Python.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MAIN = ROOT / "bridge" / "main"


def text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def between(s: str, start: str, end: str) -> str:
    i = s.index(start)
    return s[i:s.index(end, i)]


def main() -> int:
    main_c, page, mock = text(MAIN / "main.c"), text(MAIN / "web" / "index.html"), text(ROOT / "tools" / "mock_bridge.py")
    console, c5link = text(MAIN / "console.c"), text(MAIN / "c5link.h")
    errors = []

    def check(ok: bool, message: str) -> None:
        if not ok:
            errors.append(message)

    # /status fields
    fw_keys = set(re.findall(r'\\"(\w+)\\":', between(main_c, "int bridge_status_json", "\n}\n")))
    script = page.split("<script>")[1].split("</script>")[0]
    page_keys = set(re.findall(r"\bs\.([a-z][a-z0-9_]*)\b", script))
    mock_keys = set(re.findall(r'"(\w+)":', between(mock, "status = {", "}\n")))
    check(len(fw_keys) > 30, f"only {len(fw_keys)} status fields found in main.c: the check itself is broken")
    check(page_keys <= fw_keys, f"the page reads status fields the firmware does not send: {sorted(page_keys - fw_keys)}")
    check(mock_keys == fw_keys, f"mock and firmware status fields differ: only mock {sorted(mock_keys - fw_keys)}, "
                                f"only firmware {sorted(fw_keys - mock_keys)}")

    # picture commands
    fw_cmds = set(re.search(r'strchr\("(\w+)", c\)\) return false;', main_c).group(1))
    page_cmds = set(re.findall(r'data-cmd="(\w)"', page))
    mock_cmds = set("".join(re.findall(r'c (?:==|in) "(\w+)"', between(mock, "def command", "class Page"))))
    help_text = between(console, "static const char *HELP", ";\n")
    check(page_cmds <= fw_cmds, f"the page sends picture commands the firmware does not know: {sorted(page_cmds - fw_cmds)}")
    check(mock_cmds == fw_cmds, f"mock and firmware picture commands differ: mock {sorted(mock_cmds)}, firmware {sorted(fw_cmds)}")
    for c in sorted(fw_cmds):
        check(re.search(rf"(?:^|[\s\"(]){c}(?: {{1,2}}| [a-z] )", help_text) is not None,
              f"the console help does not list the picture command '{c}'")

    # receiver keys
    fw_keys_c5 = set(re.search(r'#define C5LINK_KEYS "(\w+)"', c5link).group(1))
    page_keys_c5 = set(re.findall(r'data-c5="(\w)"', page))
    mock_keys_c5 = set(re.search(r'key not in "(\w+)"', mock).group(1))
    check(page_keys_c5 <= fw_keys_c5, f"the page sends receiver keys the firmware refuses: {sorted(page_keys_c5 - fw_keys_c5)}")
    check(mock_keys_c5 == fw_keys_c5, f"mock and firmware receiver keys differ: mock {sorted(mock_keys_c5)}, "
                                      f"firmware {sorted(fw_keys_c5)}")

    for e in errors:
        print(f"  FAIL: {e}")
    print(f"consistency: {len(fw_keys)} status fields, {len(fw_cmds)} picture commands, {len(fw_keys_c5)} receiver keys: "
          + (f"{len(errors)} problem(s)" if errors else "firmware, page and mock agree"))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
