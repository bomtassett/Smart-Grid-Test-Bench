#!/usr/bin/env python3
"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     1D - Controller Network (test)
File:      End-to-end Test: Python Master vs Simulated Nodes & Load Control Unit.

Purpose:   Checks that simulated nodes and load control unit answer host's Modbus master correctly,
           over virtual serial ports and in real time.

Usage:     python3 tests/test_simnode.py   (run by make test; build/simnode is needed)
"""

import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # package root
sys.path.insert(0, str(ROOT / "host"))
from sgtb_modbus import Master, ModbusError, NoReply, load_tags, poll_node  # noqa: E402

S1, S2 = "/tmp/sgtb-test-seg1", "/tmp/sgtb-test-seg2"
fails = checks = 0  # failed checks, checks run

def check(cond, what):
    """count one check; print failure."""
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("FAIL", what)

def near(got, want, tol_pct, what):
    """check that got is within tolerance (%) of want."""
    check(abs(got - want) <= abs(want) * tol_pct / 100.0, f"{what}: {got:.5g}, want {want:.5g} within {tol_pct}%")

def main():
    """run test."""
    T = load_tags()
    sim = subprocess.Popen([str(ROOT / "build" / "simnode"), "--seg1", S1, "--seg2", S2],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def cmd(c, settle=0.45):
        """send command to simulator and wait for it to settle."""
        sim.stdin.write(c + "\n")
        sim.stdin.flush()
        time.sleep(settle)

    try:
        while "simnode:" not in sim.stdout.readline():
            pass
        time.sleep(0.6)
        m = Master(S1, 115200)

        # 12 V circuit @ HIGH (22 ohm), 24 V circuit @ 49 ohm: plan's currents
        src, sy1 = poll_node(m, T, "NODE-SRC"), poll_node(m, T, "NODE-SY1")
        near(src["SRC.MAINS.VRMS"], 12.0, 0.3, "MP-01 V")
        near(src["SRC.MAINS.IRMS"], 12.0 / 22, 1.0, "MP-01 I")
        near(sy1["SY1.LOAD.IRMS"], 12.0 / 22, 0.5, "MP-29 I")
        near(sy1["SY1.LOAD.P"], 144.0 / 22, 0.5, "MP-29 P")
        near(sy1["SY1.LOAD.FREQ"], 60.0, 0.03, "MP-29 f")
        near(src["SRC.SUBMAIN.IRMS"], 24.0 / 50, 0.5, "MP-04 I")
        near(src["SRC.SUBMAIN.VRMS"] - sy1["SY1.SUB.VRMS"], 0.48, 10.0, "line drop across the 1 ohm resistor")
        check(abs(sy1["SY1.LOAD.ANG"]) < 0.5 and sy1["SY1.LOAD.PF"] > 0.995, "resistive angle and PF")
        check(src["SRC.SYNC.VRMS"] == 0 and src["SRC.SYNC.FREQ"] == 0, "MP-03 disconnected reads zero")
        check(src["SRC.PV.VDC"] == 0 and "SRC.PV.FREQ" not in src, "DC point")
        check(int(src["SRC.HLTH.STATUS"]) & 3 == 3, "status: sampling and calibration valid")

        # load steps (Case 3), power factor, frequency, Case 4 generator
        cmd("load low")
        near(poll_node(m, T, "NODE-SY1")["SY1.LOAD.IRMS"], 0.125, 0.5, "LOW current")
        cmd("load medium")
        near(poll_node(m, T, "NODE-SY1")["SY1.LOAD.IRMS"], 12.0 / 49, 0.5, "MEDIUM current")
        cmd("load high")
        cmd("pf 0.88")
        v = poll_node(m, T, "NODE-SY1")
        check(abs(v["SY1.LOAD.PF"] - 0.88) < 0.005 and v["SY1.LOAD.Q"] > 0 and abs(v["SY1.LOAD.ANG"] - 28.36) < 0.5, "PF 0.88 lagging")
        cmd("pf 1")
        cmd("sync 7.07 56.0")
        v = poll_node(m, T, "NODE-SRC")
        near(v["SRC.SYNC.VRMS"], 7.07, 0.3, "MP-03 V")
        check(abs(v["SRC.SYNC.FREQ"] - 56.0) < 0.02, f"MP-03 f {v['SRC.SYNC.FREQ']}")
        check(abs(v["SRC.MAINS.FREQ"] - 60.0) < 0.02, "MP-01 f unaffected")

        # heartbeat and node time advance in real time; time set (HR 100..101) is followed
        a = poll_node(m, T, "NODE-SY1")
        time.sleep(1.1)
        b = poll_node(m, T, "NODE-SY1")
        check(1 <= b["SY1.HLTH.SEQ"] - a["SY1.HLTH.SEQ"] <= 2, "SEQ increments each second")
        check(900 <= b["SY1.HLTH.TIME"] - a["SY1.HLTH.TIME"] <= 1400, "TIME advances in ms")
        m.write_multiple(2, 100, [0x0001, 0x0000])  # node time is set to 65 536 ms (HR 100..101)
        time.sleep(0.25)
        t = poll_node(m, T, "NODE-SY1")["SY1.HLTH.TIME"]
        check(65536 <= t <= 65536 + 400, f"time set followed: {t}")

        # energy accumulates; one point only is zeroed by its reset bit
        e0 = poll_node(m, T, "NODE-SY1")
        check(e0["SY1.LOAD.E"] > 0 and e0["SY1.SUB.E"] > 0, "energy accumulating")
        m.write_single(2, 102, 0b10)  # point 1 (LOAD) is reset (HR 102)
        time.sleep(0.25)
        e1 = poll_node(m, T, "NODE-SY1")
        check(e1["SY1.LOAD.E"] < e0["SY1.LOAD.E"] and e1["SY1.SUB.E"] >= e0["SY1.SUB.E"], "energy reset of LOAD only")
        check(struct.unpack(">H", m.read(2, "HR", 102, 1))[0] == 0, "reset register self-clears")

        # calibration write: gain of 1.02 on LOAD current channel is read back and applied
        g = struct.unpack(">HH", struct.pack(">f", 1.02))
        m.write_multiple(2, 110 + 4 * 3, list(g))
        time.sleep(0.45)
        check(m.read(2, "HR", 110 + 4 * 3, 2) == struct.pack(">f", 1.02), "calibration readback")
        near(poll_node(m, T, "NODE-SY1")["SY1.LOAD.IRMS"], 1.02 * 12.0 / 22, 0.5, "gain applied")

        # bus time of one 10 Hz cycle through whole stack
        t0 = time.monotonic()
        for _ in range(20):
            poll_node(m, T, "NODE-SRC", energy=False)
            poll_node(m, T, "NODE-SY1", energy=False)
        per_cycle = (time.monotonic() - t0) / 20
        print(f"mean poll cycle through the simulator: {per_cycle * 1e3:.1f} ms")
        check(per_cycle < 0.05, "two-node cycle under 50 ms")

        # node lost: no reply is given by offline slave; other still answers
        cmd("offline 2", 0.1)
        try:
            poll_node(m, T, "NODE-SY1")
            check(False, "offline node answered")
        except NoReply:
            check(True, "")
        check("SRC.MAINS.VRMS" in poll_node(m, T, "NODE-SRC"), "other node unaffected")
        cmd("online 2", 0.1)
        check("SY1.LOAD.VRMS" in poll_node(m, T, "NODE-SY1"), "node back online")
        try:
            m.read(2, "IR", 60, 1)
            check(False, "unmapped register read")
        except ModbusError as e:
            check("exception 2" in str(e), "illegal address exception")
        m.close()

        # seg 2: every shed bit is acknowledged and read back (ICD-6)
        lcu = {t["tag"]: t for t in T["tags"] if t["segment"] == "seg2"}
        m2 = Master(S2, 19200)
        a = lcu["LCU.CTRL.SHED"]["slave"]
        seq = []
        for bit in range(8):
            m2.write_single(a, lcu["LCU.CTRL.SHED"]["addr"], 1 << bit)
            c, f, s = struct.unpack(">HHH", m2.read(a, "IR", lcu["LCU.STAT.CONTACTS"]["addr"], 3))
            check(c == 1 << bit and f == 0, f"shed bit {bit} readback")
            seq.append(s)
        check(len(set(seq)) > 1, "LCU heartbeat runs")
        m2.write_single(a, lcu["LCU.CTRL.SHED"]["addr"], 0)
        m2.close()
    finally:
        sim.stdin.write("quit\n")
        sim.stdin.flush()
        sim.wait(timeout=2)
    print(f"{checks} checks, {fails} failed")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
