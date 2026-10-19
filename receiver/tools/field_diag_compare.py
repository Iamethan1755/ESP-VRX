#!/usr/bin/env python3
# C5VRX-5 by Twotoz and the C5VRX contributors: compare FIELD_DIAG logs.
#
# Capture each test setup (indoor/outdoor at the same distance, low/high
# antenna, stock/other antenna, channel A/B, firmware A/B) as a serial log
# with the per-second stream on (console R), then:
#
#   python3 tools/field_diag_compare.py low.log high.log [more.log ...]
#
# Prints per log: seconds, sync evidence, gain/coherence medians, time at the
# gain maximum, chain faults and the dropouts per evidence category. These
# are receiver-side statistics, not calibrated dBm: compare runs only when
# VTX, power, channel and distance are the same, and repeat A/B/A.
import re
import statistics
import sys

KV = re.compile(r"(\w+)=(-?\d+)")


def parse(path):
    seconds, events, chain = [], [], 0
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("FIELD_DIAG t="):
                rec = {k: int(v) for k, v in KV.findall(line)}
                # Old video_pct used measurable windows as denominator.
                # Its overall coverage cannot be reconstructed: do not treat
                # that old field as new overall sync evidence.
                seconds.append(rec)
                if not (rec.get("filter_ok", 1) and rec.get("sphase_ok", 1) and rec.get("dc_ok", 1)) or rec.get("fault"):
                    chain += 1
            elif line.startswith("FIELD_DIAG_EVENT dropout"):
                m = re.search(r"category=(\w+)", line)
                d = re.search(r"duration_s=>?=?(\d+)", line)
                events.append({"category": m.group(1) if m else "?", "duration": int(d.group(1)) if d else 0})
            elif line.startswith("FIELD_DIAG_EVENT recovered") and events:
                d = re.search(r"duration_s=(\d+)", line)
                if d:
                    events[-1]["duration"] = int(d.group(1))
    return seconds, events, chain


def median(values):
    return statistics.median(values) if values else float("nan")


def summary(path):
    seconds, events, chain = parse(path)
    n = len(seconds)
    sync_known = [r for r in seconds if "sync_valid_pct" in r]
    ok = sum(1 for r in sync_known if r["sync_valid_pct"] >= 50)
    at_max = sum(1 for r in seconds if r.get("gain", 0) + 1 >= r.get("max", 255))
    cats = {}
    for e in events:
        cats[e["category"]] = cats.get(e["category"], 0) + 1
    return {
        "log": path,
        "seconds": n,
        "sync_evidence_ok_pct": 100.0 * ok / len(sync_known) if sync_known else float("nan"),
        "coverage_pct_median": median([r["measurement_coverage_pct"] for r in seconds if "measurement_coverage_pct" in r]),
        "legacy_unknown_coverage_s": sum("sync_valid_pct" not in r for r in seconds),
        "gain_median": median([r.get("gain", 0) for r in seconds]),
        "at_max_gain_pct": 100.0 * at_max / n if n else float("nan"),
        "coherence_median": median([r.get("coherence", 0) for r in seconds]),
        "cn_est_median_db": median([r["cn_est_x10"] / 10 for r in seconds if r.get("cn_est_x10", -990) > -990]),
        "noise_like_s": sum(1 for r in seconds if r.get("cn_est_x10", 0) == -990),
        "afc_step_s": sum(1 for r in seconds if r.get("afc_step")),
        "coherence_with_sync": median([r.get("coherence", 0) for r in seconds if r.get("sync_valid_pct", -1) >= 50]),
        "p50_median": median([r.get("p50", 0) for r in seconds]),
        "origin_pm_median": median([r.get("origin_pm", 0) for r in seconds]),
        "chain_fault_s": chain,
        "out_of_spec_s": sum(1 for r in seconds if r.get("in_spec", 1) == 0),
        # The kp + LP ladder at the edge (research README section 18): LP
        # below ~6.5 dB C/N, a calmer kp at 4-12 dB, kp 0.75-0.85 close in.
        # Older logs without kp_x100 report nan.
        "edge_s": len(edge := [r for r in seconds if "kp_x100" in r and -990 < r.get("cn_est_x10", -990) < 60]),
        "edge_lp_pct": 100.0 * sum(r.get("lp", 0) for r in edge) / len(edge) if edge else float("nan"),
        "edge_kp_median": median([r["kp_x100"] / 100 for r in edge if r["kp_x100"] >= 0]),
        "strong_kp_median": median([r["kp_x100"] / 100 for r in seconds
                                    if r.get("kp_x100", -1) >= 0 and r.get("cn_est_x10", -990) >= 120]),
        "dropouts": len(events),
        "dropout_s_median": median([e["duration"] for e in events]),
        "categories": cats,
    }


def main(argv):
    if len(argv) < 2:
        print("usage: field_diag_compare.py LOG [LOG ...]")
        return 2
    rows = [summary(p) for p in argv[1:]]
    keys = ["seconds", "sync_evidence_ok_pct", "coverage_pct_median", "legacy_unknown_coverage_s", "gain_median", "at_max_gain_pct", "coherence_median", "coherence_with_sync", "cn_est_median_db", "noise_like_s", "afc_step_s",
            "p50_median", "origin_pm_median", "chain_fault_s", "out_of_spec_s", "edge_s", "edge_lp_pct", "edge_kp_median", "strong_kp_median",
            "dropouts", "dropout_s_median"]
    cats = sorted({c for r in rows for c in r["categories"]})
    width = max(len(k) for k in keys + ["dropouts " + c for c in cats]) + 2
    col = max(14, *(len(r["log"]) + 2 for r in rows))
    print("".ljust(width) + "".join(r["log"].rjust(col) for r in rows))
    for k in keys:
        cells = []
        for r in rows:
            v = r[k]
            cells.append((f"{v:.1f}" if isinstance(v, float) else str(v)).rjust(col))
        print(k.ljust(width) + "".join(cells))
    for c in cats:
        print(("dropouts " + c).ljust(width) + "".join(str(r["categories"].get(c, 0)).rjust(col) for r in rows))
    if any(r["seconds"] == 0 for r in rows):
        print("note: a log without FIELD_DIAG lines; switch the stream on with console R before recording")
    if any(r["chain_fault_s"] for r in rows):
        print("note: chain faults present (filter/sampling phase/DC/transport): fix those before comparing RF setups")
    print("note: AFC sync evidence, NOT picture quality; C/N is simulation-derived, not calibrated RF power")
    print("note: receiver statistics, not dBm; compare equal VTX, power, channel and distance, repeat A/B/A")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
