#!/usr/bin/env python3
"""Issue #119 Phase8 origin-collapse sweep: capture and analyze P8ENV rows.

capture  Interactive attenuation sweep. Type a step label (e.g. the
         attenuator setting in dB) and press Enter; the script sends 'E'
         to the receiver --per-step times and tags the resulting P8ENV rows.
         An empty line repeats the previous label; 'q' ends the sweep.

analyze  Summarize one or more logs per step and test the hypothesis
         "attenuation -> central-cell occupancy -> Phase8 hard-delta tail".

Run the same sweep once on firmware gain control and once after 'N'
(native hardware AGC, zero firmware gain writes), with GOLDEN and with
Phase8, and compare the step at which std_valid/sync_q collapse.
"""

import argparse
import re
import statistics
import sys
import time

ROW_RE = re.compile(r"\bP8ENV\s+(.*)$")
STEP_RE = re.compile(r"\bP8ENV_STEP\s+label=(\S+)")
KV_RE = re.compile(r"(\w+)=(\S+)")
FAULT_KEYS = ("rx_ovf", "tx_empty", "gdma_in", "gdma_out", "bs_empty", "bs_eof")
SUPPORT_RHO = 0.6
DISPROVE_RHO = 0.2


def parse_row(text):
    row = {}
    for key, value in KV_RE.findall(text):
        if "/" in value and key.endswith("_pm"):
            row[key] = [int(v) for v in value.split("/")]
        elif value.startswith("0x"):
            row[key] = int(value, 16)
        else:
            try:
                row[key] = int(value)
            except ValueError:
                row[key] = value
    return row


def parse_log(lines):
    label = "untagged"
    rows = []
    for line in lines:
        step = STEP_RE.search(line)
        if step:
            label = step.group(1)
            continue
        match = ROW_RE.search(line)
        if match:
            row = parse_row(match.group(1))
            if "central_pm" in row and "hard_pm" in row:
                row["step"] = label
                rows.append(row)
    return rows


def ranks(values):
    order = sorted(range(len(values)), key=lambda i: values[i])
    result = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        for k in range(i, j + 1):
            result[order[k]] = (i + j) / 2.0 + 1.0
        i = j + 1
    return result


def spearman(xs, ys):
    if len(xs) < 3:
        return None
    rx, ry = ranks(xs), ranks(ys)
    mx, my = statistics.fmean(rx), statistics.fmean(ry)
    num = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    den = (sum((a - mx) ** 2 for a in rx) * sum((b - my) ** 2 for b in ry)) ** 0.5
    return num / den if den else None


def median(rows, key):
    values = [r[key] for r in rows if isinstance(r.get(key), int)]
    return statistics.median(values) if values else None


def healthy_video(row):
    return row.get("std_valid") == 1 and row.get("sync_q", 0) >= 60


def analyze(rows):
    report = {"rows": len(rows), "steps": [], "warnings": []}
    if not rows:
        report["verdict"] = "NO_DATA"
        return report

    steps = []
    for row in rows:
        if row["step"] not in steps:
            steps.append(row["step"])
    for label in steps:
        group = [r for r in rows if r["step"] == label]
        classes = {}
        for r in group:
            classes[r.get("class", "?")] = classes.get(r.get("class", "?"), 0) + 1
        report["steps"].append({
            "label": label,
            "n": len(group),
            "central_pm": median(group, "central_pm"),
            "origin_pm": median(group, "origin_pm"),
            "clip_pm": median(group, "clip_pm"),
            "p50": median(group, "p50"),
            "p95": median(group, "p95"),
            "hard_pm": median(group, "hard_pm"),
            "hard_central_pm": median(group, "hard_central_pm"),
            "hard_outer_pm": median(group, "hard_outer_pm"),
            "sync_q": median(group, "sync_q"),
            "video_ok": sum(1 for r in group if healthy_video(r)),
            "class": max(classes, key=classes.get),
            "gain_regs": len({r.get("gain_reg") for r in group}),
        })

    rho = spearman([r["central_pm"] for r in rows], [r["hard_pm"] for r in rows])
    report["rho_central_vs_hard"] = rho
    lifts = [r["hard_central_pm"] / max(r["hard_outer_pm"], 1)
             for r in rows if r.get("central_pm", 0) >= 20]
    report["hard_lift_central_over_outer"] = statistics.median(lifts) if lifts else None
    if rho is None:
        report["verdict"] = "INCONCLUSIVE(too_few_rows)"
    elif rho >= SUPPORT_RHO:
        report["verdict"] = "SUPPORTS_ORIGIN_COLLAPSE"
    elif rho <= DISPROVE_RHO:
        report["verdict"] = "DISPROVES_ORIGIN_COLLAPSE"
    else:
        report["verdict"] = "INCONCLUSIVE"

    good = [r for r in rows if healthy_video(r)]
    if good:
        hard_cut = sorted(r["hard_pm"] for r in good)[max(0, len(good) // 5 - 1)]
        best = [r for r in good if r["hard_pm"] <= hard_cut]
        report["empirical_annulus"] = {
            key: (min(r[key] for r in best), max(r[key] for r in best))
            for key in ("p50", "p95", "origin_pm", "clip_pm", "central_pm")
        }
    first_loss = next((s["label"] for s in report["steps"]
                       if s["video_ok"] * 2 < s["n"]), None)
    report["first_video_loss_step"] = first_loss

    native = [r for r in rows if r.get("native") == 1]
    if native:
        blocked = max(r.get("blocked", 0) for r in native) - min(r.get("blocked", 0) for r in native)
        report["native_rows"] = len(native)
        report["native_distinct_gain_regs"] = len({r.get("gain_reg") for r in native})
        report["native_gain_reg_changes"] = sum(r.get("gain_reg_changes", 0) for r in native)
        if blocked or any(r.get("fw_gain_epochs", 0) for r in native):
            report["warnings"].append("native capture saw firmware gain activity; rows are tainted")
    for key in FAULT_KEYS:
        values = [r[key] for r in rows if isinstance(r.get(key), int)]
        if values and max(values) != min(values):
            report["warnings"].append(f"transport fault counter {key} moved by {max(values) - min(values)}")
    return report


def fmt(value):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.2f}" if value != int(value) else str(int(value))
    return str(value)


def print_report(report):
    print(f"rows={report['rows']}")
    cols = ("label", "n", "class", "p50", "p95", "central_pm", "origin_pm", "clip_pm",
            "hard_pm", "hard_central_pm", "hard_outer_pm", "sync_q", "video_ok", "gain_regs")
    print(" ".join(f"{c:>15}" for c in cols))
    for step in report["steps"]:
        print(" ".join(f"{fmt(step[c]):>15}" for c in cols))
    print(f"spearman(central_pm, hard_pm) = {fmt(report.get('rho_central_vs_hard'))}")
    print(f"median hard lift central/outer = {fmt(report.get('hard_lift_central_over_outer'))}")
    print(f"first step with video loss = {report.get('first_video_loss_step') or '-'}")
    if "empirical_annulus" in report:
        print("empirical annulus (healthy video, lowest hard tail): " +
              " ".join(f"{k}={lo}..{hi}" for k, (lo, hi) in report["empirical_annulus"].items()))
    if "native_rows" in report:
        print(f"native rows={report['native_rows']} distinct_gain_regs={report['native_distinct_gain_regs']} "
              f"gain_reg_changes={report['native_gain_reg_changes']}")
    for warning in report["warnings"]:
        print(f"WARNING: {warning}")
    print(f"VERDICT {report['verdict']}")


def capture(args):
    import serial  # pyserial; only needed on the bench

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = args.port, 115200, 0.2
    ser.dtr = ser.rts = False
    ser.open()
    label = None
    with open(args.out, "a", encoding="utf-8") as log:
        while True:
            text = input("step label (Enter=repeat, q=quit): ").strip()
            if text.lower() == "q":
                break
            label = text or label
            if not label:
                continue
            log.write(f"P8ENV_STEP label={label}\n")
            for _ in range(args.per_step):
                ser.reset_input_buffer()
                ser.write(b"E")
                deadline = time.time() + 2.0
                while time.time() < deadline:
                    line = ser.readline().decode("utf-8", errors="replace").strip()
                    if not line:
                        continue
                    log.write(line + "\n")
                    if line.startswith("P8ENV "):
                        row = parse_row(line[6:])
                        print(f"  {label}: class={row.get('class')} central={row.get('central_pm')} "
                              f"hard={row.get('hard_pm')} p50={row.get('p50')} sync_q={row.get('sync_q')}")
                        break
                log.flush()
                time.sleep(args.interval)
    ser.close()


SELF_TEST_LOG = """
boot noise
P8ENV_STEP label=0
P8ENV native=1 blocked=0 gain_reg=0x34000000 gain_reg_changes=0 fw_gain_epochs=0 p50=20 p95=40 central_pm=2 origin_pm=10 clip_pm=0 hard_pm=3 hard_central_pm=100 hard_outer_pm=2 class=ANNULUS sync_q=90 std_valid=1 rx_ovf=0 tx_empty=0 radius_pm=0/1/2/3/4/5/6/7/8/9/10
P8ENV native=1 blocked=0 gain_reg=0x34000000 gain_reg_changes=0 fw_gain_epochs=0 p50=21 p95=41 central_pm=3 origin_pm=12 clip_pm=0 hard_pm=4 hard_central_pm=110 hard_outer_pm=2 class=ANNULUS sync_q=88 std_valid=1 rx_ovf=0 tx_empty=0
P8ENV_STEP label=20
P8ENV native=1 blocked=0 gain_reg=0x3a000000 gain_reg_changes=1 fw_gain_epochs=0 p50=9 p95=20 central_pm=60 origin_pm=180 clip_pm=0 hard_pm=40 hard_central_pm=600 hard_outer_pm=5 class=LOW sync_q=70 std_valid=1 rx_ovf=0 tx_empty=0
P8ENV native=1 blocked=0 gain_reg=0x3a000000 gain_reg_changes=0 fw_gain_epochs=0 p50=8 p95=19 central_pm=70 origin_pm=200 clip_pm=0 hard_pm=48 hard_central_pm=620 hard_outer_pm=6 class=LOW sync_q=66 std_valid=1 rx_ovf=0 tx_empty=0
P8ENV_STEP label=35
P8ENV native=1 blocked=0 gain_reg=0x3f000000 gain_reg_changes=0 fw_gain_epochs=0 p50=2 p95=6 central_pm=400 origin_pm=800 clip_pm=0 hard_pm=300 hard_central_pm=700 hard_outer_pm=10 class=COLLAPSE sync_q=10 std_valid=0 rx_ovf=0 tx_empty=0
P8ENV native=1 blocked=0 gain_reg=0x3f000000 gain_reg_changes=0 fw_gain_epochs=0 p50=2 p95=5 central_pm=450 origin_pm=850 clip_pm=0 hard_pm=320 hard_central_pm=710 hard_outer_pm=12 class=COLLAPSE sync_q=5 std_valid=0 rx_ovf=0 tx_empty=0
"""


def self_test():
    rows = parse_log(SELF_TEST_LOG.splitlines())
    assert len(rows) == 6
    assert rows[0]["radius_pm"][10] == 10 and rows[0]["gain_reg"] == 0x34000000
    report = analyze(rows)
    assert report["verdict"] == "SUPPORTS_ORIGIN_COLLAPSE", report
    assert [s["label"] for s in report["steps"]] == ["0", "20", "35"]
    assert report["first_video_loss_step"] == "35"
    assert report["native_distinct_gain_regs"] == 3
    assert not report["warnings"], report["warnings"]
    assert report["empirical_annulus"]["p50"] == (20, 20)

    inverse = [dict(r, central_pm=i * 10, hard_pm=60 - i * 10) for i, r in enumerate(rows)]
    assert analyze(inverse)["verdict"] == "DISPROVES_ORIGIN_COLLAPSE"
    tainted = analyze(parse_log(SELF_TEST_LOG.replace("blocked=0 gain_reg=0x3f", "blocked=3 gain_reg=0x3f").splitlines()))
    assert any("tainted" in w for w in tainted["warnings"])
    assert analyze([])["verdict"] == "NO_DATA"
    print("p8env_sweep: parser, correlation verdict and native taint checks passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="cmd")
    cap = sub.add_parser("capture")
    cap.add_argument("--port", required=True)
    cap.add_argument("--out", default="p8env_sweep.log")
    cap.add_argument("--per-step", type=int, default=5)
    cap.add_argument("--interval", type=float, default=0.5)
    ana = sub.add_parser("analyze")
    ana.add_argument("logs", nargs="+")
    args = parser.parse_args()

    if args.self_test:
        self_test()
    elif args.cmd == "capture":
        capture(args)
    elif args.cmd == "analyze":
        lines = []
        for path in args.logs:
            with open(path, encoding="utf-8", errors="replace") as handle:
                lines.extend(handle.readlines())
        print_report(analyze(parse_log(lines)))
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
