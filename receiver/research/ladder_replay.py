#!/usr/bin/env python3
"""C5VRX by Twotoz and the C5VRX contributors.

Replays kp_ladder_update_lp() + kp_switch_confirm_toward() (wide class,
deviation unknown, allow_lp) on the board walk's FIELD_DIAG C/N, each 1 s
value held for its 20 control windows of 50 ms, every window usable. This
is the best case for the guard: if it confirms here and the board did not,
something outside the C/N held it. Integer arithmetic as in C.
Usage: python research/ladder_replay.py [log] [t0] [t1] [p_bad] [old|new]
p_bad: share of windows made unusable at random (seeded). old: an unusable
window clears the pending change and the guard candidate (PR 211 as walked);
new: it holds them, gaps over 500 ms restart (KP_STALE_US, KP_SWITCH_STALE_US).
"""
import re, sys, os, random
D = os.path.join(os.path.dirname(__file__), "data", "walk_2026-10-13")
log = sys.argv[1] if len(sys.argv) > 1 else os.path.join(D, "walk_serial.log")
t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 57; t1 = float(sys.argv[3]) if len(sys.argv) > 3 else 171
p_bad = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
HOLD_GAPS = len(sys.argv) > 5 and sys.argv[5] == 'new'
STALE = 500000
rng = random.Random(1)
CN_UNKNOWN = -990
CNP = [0, 20, 40, 80, 120, 200]
K = [85, 85, 80, 80, 80, 80]; KLP = [85] * 6           # wide rows (12 MHz class)
LP_ON, LP_FLOOR, HYST = 85, 15, 15
HOLD, GAP, HOLD_UP, GAP_UP, LP_HOLD, LP_GAP, CONFIRM = 1e6, 5e5, 3e5, 2e5, 1e6, 3e6, 2e6
# Rescue timing (kp_ladder.h / kp_switch_guard.h); env overrides for studies.
ENTER_HOLD = float(os.environ.get('ENTER_HOLD', 3e5)); ENTER_GAP = float(os.environ.get('ENTER_GAP', 5e5))
RELEASE_HOLD = float(os.environ.get('RELEASE_HOLD', 5e6)); RESCUE_CONFIRM = float(os.environ.get('RESCUE_CONFIRM', 2e5))
SWITCH_GAP, RESCUE_GAP = 5e6, 5e5
ADAPTIVE = os.environ.get('ADAPTIVE', '1') == '1'   # firmware: release hold 2 s, doubled on re-entry within 30 s, max 16 s
def tdiv(a, b): q = abs(a) // abs(b); return q if (a >= 0) == (b > 0) else -q
def curve(cn, lp):
    k = KLP if lp else K
    if cn <= CNP[0]: return k[0]
    for i in range(1, 6):
        if cn <= CNP[i]: return k[i-1] + tdiv((k[i]-k[i-1]) * (cn-CNP[i-1]), CNP[i]-CNP[i-1])
    return k[5]
def level_for(kp): lv = tdiv(85 - kp + 2, 5); return max(0, min(7, lv))
def want_lp(cn, lp): return (cn < LP_ON + HYST and cn >= LP_FLOOR) if lp else (cn < LP_ON and cn >= LP_FLOOR + HYST // 3)

L = dict(level=0, lp=False, lp_pending=-1, lp_since=0, lp_switch=-(1 << 40), cn_q4=0, init=False,
         pdir=0, psince=0, last_step=0, steps=0, lp_switches=0)
G = dict(cand=False, side=0, lp=False, since=0)
applied = (0, False); events = []
fd = []
for line in open(log, encoding="utf-8"):
    m = re.match(r"\s*([\d.]+) \| FIELD_DIAG t=\d+ t_ms=(\d+) .*cn_est_x10=(-?\d+)", line)
    if m and t0 <= float(m.group(1)) <= t1:
        sv = int(re.search(r"sync_valid_pct=(\d+)", line).group(1))
        fd.append((float(m.group(1)), int(m.group(3)), sv))
now = 0
lp_time = {"good": 0, "bad": 0}
for ts, cn_raw, sync in fd:
    if applied[1]: lp_time["good" if sync >= 50 else "bad"] += 1
    for w in range(20):
        now += 50000
        bad = rng.random() < p_bad
        if HOLD_GAPS:
            if now - L.get("usable", 0) > STALE: L["pdir"] = 0; L["lp_pending"] = -1
            if G["cand"] and now - G.get("valid", 0) > STALE: G["cand"] = False
            if bad: continue
            L["usable"] = G["valid"] = now
        elif bad:                                    # !ladder_ok: hold, clear pending and candidate
            L["pdir"] = 0; L["lp_pending"] = -1; G["cand"] = False; continue
        raw = 0 if cn_raw <= CN_UNKNOWN else cn_raw
        if not L["init"]: L["cn_q4"] = raw * 16; L["init"] = True
        else: L["cn_q4"] += tdiv(raw * 16 - L["cn_q4"], 8)
        cn = tdiv(L["cn_q4"], 16)
        want = 1 if want_lp(cn, L["lp"]) else 0
        if want == int(L["lp"]): L["lp_pending"] = -1
        elif want != L["lp_pending"]: L["lp_pending"] = want; L["lp_since"] = now
        elif now - L["lp_since"] >= (ENTER_HOLD if want else (L.get("rel", 2e6) if ADAPTIVE else RELEASE_HOLD)) and now - L["lp_switch"] >= (ENTER_GAP if want else LP_GAP):
            if ADAPTIVE and want:
                L["rel"] = min(2 * L.get("rel", 2e6), 16e6) if now - L.get("released", -1e12) < 30e6 else 2e6
            if ADAPTIVE and not want: L["released"] = now
            L["lp"] = bool(want); L["lp_pending"] = -1; L["lp_switch"] = now; L["lp_switches"] += 1
            L["level"] = level_for(curve(cn, L["lp"])); L["pdir"] = 0; L["last_step"] = now; L["steps"] += 1
            events.append((ts, f"internal LP -> {int(L['lp'])} (cn {cn/10:.1f} dB), lp_switches={L['lp_switches']}"))
        else:
            tgt = level_for(curve(cn, L["lp"]))
            d = (tgt > L["level"]) - (tgt < L["level"])
            if d != L["pdir"]: L["pdir"] = d; L["psince"] = now
            elif d and now - L["psince"] >= (HOLD_UP if d < 0 else HOLD) and now - L["last_step"] >= (GAP_UP if d < 0 else GAP):
                L["level"] += d; L["last_step"] = now; L["steps"] += 1
        side = 2 if L["lp"] != applied[1] else (L["level"] > applied[0]) - (L["level"] < applied[0])
        if side == 0: G["cand"] = False; continue
        if not G["cand"] or G["side"] != side or G["lp"] != L["lp"]:
            G.update(cand=True, side=side, lp=L["lp"], since=now); continue
        rescue = side == 2 and L["lp"]
        if now - G["since"] >= (RESCUE_CONFIRM if rescue else CONFIRM) and                 now - G.get("last_apply", -1e12) >= (RESCUE_GAP if rescue else SWITCH_GAP):
            G["last_apply"] = now
            events.append((ts, f"APPLY kp_level={L['level']} lp={int(L['lp'])}  (board: nothing applied)"))
            applied = (L["level"], L["lp"]); G["cand"] = False
for ts, e in events: print(f"{ts:7.1f} s  {e}")
print(f"LP applied while sync>=50%: {lp_time['good']} s, while sync<50%: {lp_time['bad']} s")
print(f"replay: steps={L['steps']} lp_switches={L['lp_switches']}, applies={sum('APPLY' in e for _, e in events)}")
