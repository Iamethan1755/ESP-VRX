#!/usr/bin/env python3
"""C5VRX by Twotoz and the C5VRX contributors.

Board walk of 2026-10-13 (research/data/walk_2026-10-13): what the gain,
the C/N estimate and the kp/LP ladder did, from FIELD_DIAG (1/s),
FIELD_TRACE (50 ms windows around dropouts) and `T` lines.
Usage: python research/field_walk_analysis.py [data dir]
"""
import re, sys, os, statistics as st
from collections import Counter

D = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "data", "walk_2026-10-13")
RUNS = [("boot1 walk (VTX on ~57-171 s)", "walk_serial.log", (57, 171)),
        ("boot2 edge", "edge_serial.log", None), ("boot2 edge (2)", "edge2_serial.log", None)]
KV = re.compile(r"(\w+)=(-?\d+)")
CN_UNKNOWN = -990
LP_ON, LP_FLOOR, HYST = 85, 15, 15        # kp_ladder.h, wide class (deviation unknown)

def rows(path, tag):
    out = []
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*([\d.]+) \| (\S+)", line)
        if not m or m.group(2) != tag: continue
        d = {k: int(v) for k, v in KV.findall(line)}
        d["ts"] = float(m.group(1)); out.append(d)
    return out

def want_lp(cn, lp):                    # kp_ladder_want_lp_g, wide class
    return cn < LP_ON + HYST and cn >= LP_FLOOR if lp else cn < LP_ON and cn >= LP_FLOOR + HYST // 3

def runs(flags):
    r, n = [], 0
    for f in flags:
        if f: n += 1
        elif n: r.append(n); n = 0
    if n: r.append(n)
    return r

for name, fn, win in RUNS:
    p = os.path.join(D, fn)
    if not os.path.exists(p): continue
    fd = [r for r in rows(p, "FIELD_DIAG") if "gain" in r and "t_ms" in r]
    if win: fd = [r for r in fd if win[0] <= r["ts"] <= win[1]]
    tr = [r for r in rows(p, "FIELD_TRACE") if "t_ms" in r]
    ctl = rows(p, "CONTROL"); tp = rows(p, "TRANSPORT")
    print(f"\n== {name}: {len(fd)} s FIELD_DIAG, {len(tr)} FIELD_TRACE windows")
    g = [r["gain"] for r in fd]
    print(f"gain: min {min(g)} median {st.median(g)} max {max(g)} (table max {fd[0]['max']}); "
          f"at max {sum(x == fd[0]['max'] for x in g)} s; >= 70 {sum(x >= 70 for x in g)} s")
    sync = [r for r in fd if r["sync_valid_pct"] > 0]; nos = [r for r in fd if r["sync_valid_pct"] == 0]
    def cnq(rs):
        v = sorted(r["cn_est_x10"] for r in rs)
        unk = sum(x == CN_UNKNOWN for x in v); k = [x for x in v if x != CN_UNKNOWN]
        q = (k[len(k) // 4], k[len(k) // 2], k[3 * len(k) // 4]) if k else ("-",) * 3
        return f"{len(v)} s, unknown {unk}, cn_x10 q25/50/75 {q}"
    print(f"sync>0: {cnq(sync)}\nsync=0: {cnq(nos)}")
    dg = [b["gain_writes"] - a["gain_writes"] for a, b in zip(fd, fd[1:])]
    print(f"gain writes/s: median {st.median(dg)}, max {max(dg)}, seconds > 20/s: {sum(x > 20 for x in dg)}")
    lp_ok = [want_lp(0 if r["cn_est_x10"] == CN_UNKNOWN else r["cn_est_x10"], False) for r in fd]
    rl = runs(lp_ok)
    print(f"LP wanted (wide class, 1 s samples): {sum(lp_ok)} s in {len(rl)} runs, longest {max(rl) if rl else 0} s "
          f"(hold 1 s + confirm 2 s needs >= 3 s)")
    if tr:
        c = [0 if r["cn_est_x10"] == CN_UNKNOWN else r["cn_est_x10"] for r in tr]
        o = Counter("unknown" if r["cn_est_x10"] == CN_UNKNOWN else "<2dB" if r["cn_est_x10"] < 20 else
                    "2-8.5dB" if r["cn_est_x10"] < 85 else ">=8.5dB" for r in tr)
        print(f"50 ms windows around dropouts: {dict(o)}")
    if ctl:
        f, l = ctl[0], ctl[-1]
        print(f"ladder internal: kp_steps {f['kp_steps']} -> {l['kp_steps']}, lp_switches {f['lp_switches']} -> {l['lp_switches']}; "
              f"afc_locked ever {max(r['afc_locked'] for r in ctl)}, deviation_x100 {l['deviation_x100']}")
    if tp:
        print(f"applied: prog_switches {tp[-1]['prog_switches']}, kp_switches {tp[-1]['kp_switches']}, kp_level {tp[-1]['kp_level']}")
