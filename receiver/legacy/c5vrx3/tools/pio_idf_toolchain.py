"""PlatformIO pre-script: build with the compiler that ESP-IDF requires.

ESP-IDF refuses to configure unless ``riscv32-esp-elf-gcc`` is exactly the
version listed in ``tools/tools.json`` of the framework package
(``esp-15.2.0_20251204`` for ESP-IDF 6.0.x). The pioarduino platform ships the
toolchain of its Arduino/ESP-IDF 5.5 release (GCC 14.2) and re-installs that
version whenever something else is pinned through ``platform_packages``, so
this script installs the required toolchain the way ``idf.py`` does -- with
ESP-IDF's own ``idf_tools.py``, checksum-verified, into ``IDF_TOOLS_PATH``
(the pioarduino platform points that at the PlatformIO core directory, so the
toolchain lands in ``~/.platformio/tools/riscv32-esp-elf/<version>``) -- and
makes the PlatformIO builder use it.

Nothing happens when the platform already provides the exact version.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

from platformio.platform.base import PlatformBase

Import("env")  # noqa: F821 - provided by the PlatformIO SCons environment

TOOL = "riscv32-esp-elf"
PACKAGE = "toolchain-riscv32-esp"


def _package_version(package_dir):
    try:
        with open(Path(package_dir) / "package.json", encoding="utf-8") as fp:
            return json.load(fp).get("version")
    except (OSError, ValueError):
        return None


def _required_tool(framework_dir):
    tools_json = framework_dir / "tools" / "tools.json"
    with open(tools_json, encoding="utf-8") as fp:
        tools = json.load(fp)["tools"]
    tool = next(t for t in tools if t["name"] == TOOL)
    version = next(v["name"] for v in tool["versions"] if v.get("status") == "recommended")
    return tools_json, tool, version


def _install(python, framework_dir, tools_json, tools_path):
    idf_env = dict(os.environ)
    idf_env["IDF_TOOLS_PATH"] = str(tools_path)
    idf_env.pop("MSYSTEM", None)  # idf_tools.py refuses to run inside MSYS shells
    cmd = [
        python,
        str(framework_dir / "tools" / "idf_tools.py"),
        "--non-interactive",
        "--tools-json",
        str(tools_json),
        "--idf-path",
        str(framework_dir),
        "install",
        TOOL,
    ]
    print(f"Installing {TOOL} for ESP-IDF into {tools_path} (idf_tools.py)")
    result = subprocess.run(cmd, env=idf_env, check=False)
    if result.returncode != 0:
        sys.stderr.write(f"Error: idf_tools.py failed to install {TOOL} (exit {result.returncode})\n")
        env.Exit(1)


def _use_toolchain(toolchain_dir, bin_dir):
    original = PlatformBase.get_package_dir

    def get_package_dir(self, name):
        if name == PACKAGE:
            return str(toolchain_dir)
        return original(self, name)

    PlatformBase.get_package_dir = get_package_dir
    env.PrependENVPath("PATH", str(bin_dir))


def main():
    if env.IsCleanTarget():
        return
    platform = env.PioPlatform()
    framework_dir = Path(platform.get_package_dir("framework-espidf"))
    tools_json, tool, version = _required_tool(framework_dir)

    # PlatformIO spells "esp-15.2.0_20251204" as "15.2.0+20251204".
    if _package_version(platform.get_package_dir(PACKAGE)) == version.replace("esp-", "").replace("_", "+"):
        return

    tools_path = Path(os.environ.get("IDF_TOOLS_PATH") or os.path.expanduser("~/.espressif"))
    bin_dir = tools_path.joinpath("tools", TOOL, version, *tool["export_paths"][0])
    gcc = bin_dir / (f"{TOOL}-gcc.exe" if os.name == "nt" else f"{TOOL}-gcc")
    if not gcc.is_file():
        _install(env.subst("$PYTHONEXE"), framework_dir, tools_json, tools_path)
    if not gcc.is_file():
        sys.stderr.write(f"Error: {gcc} is missing after installing {TOOL} {version}\n")
        env.Exit(1)

    print(f"Using {TOOL} {version} from {bin_dir.parent}")
    _use_toolchain(bin_dir.parent, bin_dir)


main()
