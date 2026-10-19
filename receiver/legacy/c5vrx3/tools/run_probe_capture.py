#!/usr/bin/env python3
"""Automated PHY Phase-Tap Probe runner and analyzer for C5VRX-3."""

import subprocess
import sys
import time
import serial


def capture_probe(port: str = "COM10", baud: int = 115200, timeout_sec: float = 25.0) -> bytes:
    print(f"Opening {port} at {baud} baud...")
    s = serial.Serial(port, baud, timeout=0.2)
    s.dtr = True
    s.rts = False
    time.sleep(0.1)

    # Drain buffer
    if s.in_waiting:
        s.read(s.in_waiting)

    print("Triggering reboot on C5VRX ('K')...")
    s.write(b"K")

    captured = b""
    t0 = time.time()
    while time.time() - t0 < timeout_sec:
        try:
            n = s.in_waiting
            if n:
                c = s.read(n)
                captured += c
                if b"PHY_TAP END" in captured:
                    print("Received PHY_TAP END delimiter!")
                    break
        except serial.SerialException:
            time.sleep(0.5)
            for _ in range(30):
                try:
                    s = serial.Serial(port, baud, timeout=0.2)
                    s.dtr = True
                    s.rts = False
                    break
                except Exception:
                    time.sleep(0.1)
        time.sleep(0.05)

    s.close()
    return captured


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
    out_file = sys.argv[2] if len(sys.argv) > 2 else "tools/capture_phase_tap.log"

    print(f"=== C5VRX PHY PHASE-TAP PROBE (Port: {port}) ===")
    captured = capture_probe(port)
    print(f"Total bytes captured: {len(captured)}")

    with open(out_file, "wb") as f:
        f.write(captured)
    print(f"Saved capture to: {out_file}")

    print("\n=== RUNNING PHY TAP ANALYZER ===")
    res = subprocess.run([sys.executable, "tools/analyze_phy_phase_tap.py", out_file], text=True)
    if res.returncode != 0:
        print("PHY Tap Analyzer exited with error.")

    if b"DIAG_SWEEP BEGIN" in captured:
        print("\n=== RUNNING 32-LANE MODEM_DIAG SWEEP ANALYZER ===")
        res_sweep = subprocess.run([sys.executable, "tools/analyze_all_diag.py", out_file], text=True)
        if res_sweep.returncode != 0:
            print("32-Lane Sweep Analyzer exited with error.")

    if b"Q6_DUMP BEGIN" in captured:
        print("\n=== RUNNING Q6 ALIGNED DUMP ANALYZER ===")
        res_q6 = subprocess.run([sys.executable, "tools/analyze_q6_dump.py", out_file], text=True)
        if res_q6.returncode != 0:
            print("Q6 Analyzer exited with error.")


if __name__ == "__main__":
    main()
