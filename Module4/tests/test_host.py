#!/usr/bin/env python3
"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     2F/2G - Communication Servers / Applications (test)
File:      End-to-end Test: Host Poller & Historian.

Purpose:   Checks host poller and historian against simulator: live values, alarms, loss
           and return of node, load-control command, what is stored, restart, and exports.

Usage:     python3 tests/test_host.py   (run by make test; build/simnode is needed)
"""

import csv
import os
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # package root
sys.path.insert(0, str(ROOT / "host"))
from historian import GOOD, STALE, Historian  # noqa: E402
from poller import Poller  # noqa: E402

S1, S2 = "/tmp/sgtb-host-seg1", "/tmp/sgtb-host-seg2"
fails = checks = 0  # failed checks, checks run

def check(cond, what):
    """count one check; print failure."""
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("FAIL", what)

def wait_for(pred, timeout):
    """wait until pred is true; return time taken, or None on timeout."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if pred():
            return time.monotonic() - t0
        time.sleep(0.02)
    return None

def main():
    """run test."""
    tmp = tempfile.mkdtemp(prefix="sgtb-")
    db = os.path.join(tmp, "hist.db")
    sim = subprocess.Popen([str(ROOT / "build" / "simnode"), "--seg1", S1, "--seg2", S2],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def cmd(c):
        """send command to simulator."""
        sim.stdin.write(c + "\n")
        sim.stdin.flush()

    over = dict(ports={"seg1": S1, "seg2": S2}, database=db, historian={"trend_period_s": 2},
                alarms={"source_active": "solar"})
    try:
        while "simnode:" not in sim.stdout.readline():
            pass
        p = Poller(**over)
        p.start()
        t_start = time.monotonic()
        time.sleep(3.2)

        # live tier, derived quantities, loop timing
        tags, alarms = p.snapshot()
        val = lambda n: tags[n][0]
        check(abs(val("SY1.LOAD.VRMS") - 12.0) < 0.05 and tags["SY1.LOAD.VRMS"][2] == GOOD, "live value and quality")
        check(abs(val("SYS.GEN.P") - 144 / 22) < 0.07 and abs(val("SYS.CONS.P") - 144 / 22) < 0.05, "generation and consumption")
        check(abs(val("SYS.BAL.P") - (val("SYS.GEN.P") - val("SYS.CONS.P"))) < 1e-9, "balance")
        check(abs(val("SY1.LINE.VDROP") - 0.48) < 0.03, f"line drop {val('SY1.LINE.VDROP')}")
        check(abs(val("SY1.LINE.PLOSS") - 0.48 ** 2) < 0.01, "line loss")
        s = p.stats
        check(29 <= s["cycles"] <= 34 and s["overruns"] == 0, f"10 Hz loop: {s['cycles']} cycles, {s['overruns']} overruns")
        print(f"bus time per cycle: mean {1e3 * s['bus_s'] / s['cycles']:.1f} ms, worst {1e3 * s['max_bus_s']:.1f} ms")
        check(abs(tags["SY1.LOAD.VRMS"][1] - time.time() * 1000) < 1500, "node time stamps follow host time")
        # MP-03 is read as 0 w/ solar source selected, so inverter voltage alert is active
        check(("voltage out of band", "SRC.SYNC") in alarms, "inverter voltage alert at 0 V")

        # node firmware version (IR 80) is compared w/ host's register map version @ each 1 Hz read
        check(val("SRC.HLTH.FWVER") == p.fw_expect == 0x0002, "node firmware version read and expected")
        check(not any(a[0] == "firmware mismatch" for a in alarms), "no firmware mismatch with matching versions")
        p.fw_expect = 0x0003
        check(wait_for(lambda: ("firmware mismatch", "NODE-SRC") in p.snapshot()[1], 2.5) is not None, "firmware mismatch reported")
        p.fw_expect = 0x0002
        check(wait_for(lambda: not any(a[0] == "firmware mismatch" for a in p.snapshot()[1]), 2.5) is not None, "firmware mismatch cleared")

        # Case 4: generator on MP-03 @ 7.07 V; in band @ 60 Hz, out @ 64 Hz
        cmd("sync 7.07 60")
        time.sleep(1.5)
        check(("frequency out of band", "SRC.SYNC") not in p.snapshot()[1], "no frequency alert at 60 Hz")
        check(("voltage out of band", "SRC.SYNC") in p.snapshot()[1], "voltage alert at 7.07 V")
        cmd("sync 7.07 64")
        dt = wait_for(lambda: ("frequency out of band", "SRC.SYNC") in p.snapshot()[1], 4)
        print(f"frequency alert raised after {dt:.2f} s" if dt else "frequency alert not raised")
        check(dt is not None and dt < 3.0, "frequency alert within 3 s (CAP-M4-47)")
        cmd("sync 12 60")
        dt = wait_for(lambda: not [a for a in p.snapshot()[1] if a[1] == "SRC.SYNC"], 4)
        check(dt is not None, "alerts clear when the input returns in band")

        # node is declared lost within 3 s; its values are marked stale w/ last-good time; then it recovers
        cmd("offline 2")
        t_off = time.monotonic()
        dt_stale = wait_for(lambda: p.snapshot()[0]["SY1.LOAD.VRMS"][2] == STALE, 2)
        dt = wait_for(lambda: ("node offline", "NODE-SY1") in p.snapshot()[1], 5)
        lost_after = time.monotonic() - t_off
        print(f"stale after {dt_stale:.2f} s, node-offline alarm after {lost_after:.2f} s")
        check(dt is not None and lost_after < 3.5, "node lost detected within 3 s plus one cycle")
        tags, _ = p.snapshot()
        check(tags["SY1.LOAD.VRMS"][3] is not None and tags["SYS.CONS.P"][2] == STALE, "last-good time kept; derived tag stale")
        check(tags["SRC.MAINS.VRMS"][2] == GOOD, "other node unaffected")
        cmd("online 2")
        dt = wait_for(lambda: ("node offline", "NODE-SY1") not in p.snapshot()[1] and p.snapshot()[0]["SYS.CONS.P"][2] == GOOD, 3)
        check(dt is not None, "node recovers")

        # load-control command: acknowledged, confirmed by readback, and recorded
        r = p.shed(0b00001010, user="operator1").get(timeout=2)
        print(f"shed command acknowledged in {1e3 * r['ack_s']:.1f} ms")
        check(r["acked_at"] is not None and r["ack_s"] < 0.5, "acknowledged within 500 ms (CAP-M4-37)")
        dt = wait_for(lambda: p.snapshot()[0]["LCU.STAT.CONTACTS"][0] == 0b1010, 1)
        check(dt is not None, "contacts follow within 1 s")
        time.sleep(0.3)
        cmd("offline 11")
        r2 = p.shed(0, user="operator1").get(timeout=3)
        check(r2["acked_at"] is None and r2["result"] == "no reply" and 0.45 < r2["ack_s"] < 0.75, f"no reply after 500 ms: {r2}")
        dt = wait_for(lambda: ("comm loss", "LCU") in p.snapshot()[1], 2)
        check(dt is not None, "comm loss alarm")
        cmd("online 11")
        time.sleep(0.5)
        t_run = time.monotonic() - t_start
        p.stop()

        # historian content
        con = sqlite3.connect(db)
        q = lambda sql, *a: con.execute(sql, a).fetchall()
        n_analog = q("SELECT COUNT(*) FROM tag WHERE class='analog'")[0][0]
        n_samp = q("SELECT COUNT(*), COUNT(DISTINCT ts) FROM sample_1hz")[0]
        print(f"historian: {n_analog} analog tags, {n_samp[0]} samples at {n_samp[1]} instants, "
              f"{q('SELECT COUNT(*) FROM trend_1min')[0][0]} trend rows, journal mode {q('PRAGMA journal_mode')[0][0]}")
        check(q("PRAGMA journal_mode")[0][0] == "wal", "write-ahead-log mode")
        check(t_run - 2 <= n_samp[1] <= t_run + 1 and n_samp[0] >= n_samp[1] * (n_analog - 6), f"1 Hz samples for every analog tag: {n_samp} in {t_run:.1f} s")
        check(q("SELECT COUNT(*) FROM sample_1hz WHERE quality=1")[0][0] > 0, "stale samples recorded as stale")
        tr = q("SELECT avg, min, max, n_good FROM trend_1min t JOIN tag USING(tag_id) WHERE name='SRC.MAINS.VRMS'")
        check(len(tr) >= 5 and all(lo <= a <= hi and n >= 1 for a, lo, hi, n in tr), "trend rows: min <= avg <= max")
        cm = q("SELECT user_id, value, result, reported_state FROM command ORDER BY command_id")
        check(cm == [("operator1", 10, "acknowledged", 10), ("operator1", 0, "no reply", None)], f"command rows {cm}")
        al = {(c, t): (r is not None, cl is not None) for c, t, r, cl in q("SELECT class, target, raised_at, cleared_at FROM alarm")}
        check(al.get(("node offline", "NODE-SY1")) == (True, True) and al.get(("frequency out of band", "SRC.SYNC")) == (True, True),
              "alarms recorded with raise and clear times")
        check(q("SELECT COUNT(*) FROM state_change s JOIN tag USING(tag_id) WHERE name='LCU.STAT.CONTACTS'")[0][0] >= 2, "contact state changes")
        check(q("SELECT COUNT(*) FROM audit WHERE action='shed'")[0][0] == 2, "audit entries for commands")
        last_ts = q("SELECT MAX(ts) FROM sample_1hz")[0][0]
        con.close()

        # restart: data survives, tag ids are stable, and time never steps backward (CAP-M4-54, CST-M4-16)
        p = Poller(**over)
        p.start()
        time.sleep(2.3)
        p.stop()
        con = sqlite3.connect(db)
        after = con.execute("SELECT COUNT(*), MIN(ts) FROM sample_1hz WHERE ts > ?", (last_ts,)).fetchone()
        check(after[0] > 0 and con.execute("SELECT COUNT(*) FROM sample_1hz WHERE ts <= ?", (last_ts,)).fetchone()[0] == n_samp[0],
              "earlier data intact and new data appended after restart")
        check(con.execute("SELECT COUNT(*) FROM tag").fetchone()[0] == con.execute("SELECT COUNT(DISTINCT name) FROM tag").fetchone()[0], "tags not duplicated")
        con.close()

        # exports: CSV of one tag (ICD-5_5.4) and one-file snapshot (CAP-M4-58)
        h = Historian(db)
        out = os.path.join(tmp, "SY1.LOAD.VRMS.csv")
        t0 = time.monotonic()
        n = h.export_csv("SY1.LOAD.VRMS", 0, h.now_ms(), out, user="operator1")
        rows = list(csv.reader(open(out)))
        check(rows[0] == ["ts", "tag", "value", "unit", "quality"] and len(rows) == n + 1 and n >= 12, "CSV export")
        check(rows[1][0].endswith("Z") and rows[1][3] == "V" and {r[4] for r in rows[1:]} <= {"good", "stale"}, "CSV columns")
        snap = os.path.join(tmp, "handover.db")
        h.snapshot(snap)
        h.close()
        print(f"CSV export of {n} rows and snapshot in {1e3 * (time.monotonic() - t0):.0f} ms")
        c2 = sqlite3.connect(snap)
        check(c2.execute("SELECT COUNT(*) FROM sample_1hz").fetchone()[0] >= n_samp[0] and
              c2.execute("SELECT COUNT(*) FROM command").fetchone()[0] == 2, "snapshot holds the store in one file")
        c2.close()
    finally:
        sim.stdin.write("quit\n")
        sim.stdin.flush()
        sim.wait(timeout=2)
    print(f"{checks} checks, {fails} failed")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
