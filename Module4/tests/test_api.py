#!/usr/bin/env python3
"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     2G - Applications (test)
File:      End-to-end Test: Supervisory API & Demand Controller.

Purpose:   Checks web interface and automatic load shedding against simulator, as client would
           use them: login and roles, live stream, commands, configuration, automatic shedding
           and restoration, and audit trail.

Usage:     python3 tests/test_api.py   (run by make test; build/simnode is needed)
"""

import os
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # package root
sys.path.insert(0, str(ROOT / "host"))
from fastapi.testclient import TestClient  # noqa: E402
from starlette.websockets import WebSocketDisconnect  # noqa: E402

from api import create_app  # noqa: E402
from poller import Poller  # noqa: E402

S1, S2 = "/tmp/sgtb-api-seg1", "/tmp/sgtb-api-seg2"
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

    p = None
    try:
        while "simnode:" not in sim.stdout.readline():
            pass
        p = Poller(ports={"seg1": S1, "seg2": S2}, database=db)
        p.start()
        app = create_app(p, admin_password="admin-pass-1")
        c = TestClient(app)
        time.sleep(1.5)

        def login(user, pw):
            """log in; return status code and session header."""
            r = c.post("/api/login", json={"user": user, "password": pw})
            return r.status_code, ({"Authorization": "Bearer " + r.json()["session"]} if r.status_code == 200 else None)

        # ---- no anonymous access (CST-M4-05)
        c.cookies.clear()
        check(c.get("/api/snapshot").status_code == 401, "snapshot without login")
        check(c.post("/api/commands/shed", json={"action": "next", "confirm": True}).status_code == 401, "command without login")
        try:
            with c.websocket_connect("/api/live") as ws:
                ws.receive_json()
            check(False, "stream opened without login")
        except WebSocketDisconnect as e:
            check(e.code == 4401, "stream refused without login")
        check(p.commanded() == 0, "no contact changed")

        # ---- users and roles
        code, admin = login("admin", "admin-pass-1")
        check(code == 200, "administrator login")
        c.cookies.clear()
        mk = lambda n, r, pw: c.post("/api/users", headers=admin, json={"name": n, "role": r, "password": pw, "confirm": True})
        check(mk("viewer1", "Viewer", "viewer-pass").status_code == 200 and mk("oper1", "Operator", "oper-pass-1").status_code == 200, "users created")
        check(mk("oper1", "Operator", "oper-pass-1").status_code == 409 and mk("x", "Operator", "short").status_code == 422, "duplicate and weak password refused")
        _, viewer = login("viewer1", "viewer-pass")
        _, oper = login("oper1", "oper-pass-1")
        c.cookies.clear()
        check(login("viewer1", "wrong")[0] == 401, "wrong password")
        c.cookies.clear()

        # ---- Viewer: reads are allowed; every control is refused w/ 403
        snap = c.get("/api/snapshot", headers=viewer).json()
        tag = {t["name"]: t for t in snap["tags"]}
        check(abs(tag["SY1.LOAD.VRMS"]["value"] - 12.0) < 0.05 and tag["SY1.LOAD.VRMS"]["quality"] == "good" and tag["SY1.LOAD.VRMS"]["unit"] == "V", "snapshot tag value")
        check(snap["auto"]["shedding"] is False and abs(snap["auto"]["limit_a"] - 0.8 * 2.9) < 1e-9, "automatic control off; limit 0.80 x 2.9 A")
        denied = [c.post("/api/commands/shed", headers=viewer, json={"action": "next", "confirm": True}).status_code,
                  c.post("/api/metering/SY1/reset", headers=viewer, json={"confirm": True}).status_code,
                  c.put("/api/config/i_s_max", headers=viewer, json={"value": 1, "confirm": True}).status_code,
                  c.put("/api/control/auto", headers=viewer, json={"shedding": True, "confirm": True}).status_code,
                  c.get("/api/users", headers=viewer).status_code,
                  c.get("/api/users", headers=oper).status_code]
        check(denied == [403] * 6 and p.commanded() == 0, f"role checks: {denied}")
        bal = c.get("/api/balance", headers=viewer).json()
        check(abs(bal["generation"] - 144 / 22) < 0.07 and abs(bal["cons_by_load"]["SY1"] - 144 / 22) < 0.05 and
              abs(bal["balance"] - (bal["generation"] - bal["consumption"])) < 1e-9, "balance")
        check(c.get("/api/balance?window=1h", headers=viewer).json()["generation"] is not None, "balance from the historian")
        m = c.get("/api/metering", headers=viewer).json()
        check(m[0]["load_id"] == "SY1" and m[0]["e"] > 0 and m[0]["last_reset_ts"] is None, "metering")
        card = c.get("/api/loads", headers=viewer).json()[0]
        check(card["name"] == "West Ashley" and [x["bit"] for x in card["contacts"]] == [1, 3] and card["faults"] == [] and card["i_cap"] == 0.6, "load card")
        tr = c.get("/api/trend?tags=SY1.LOAD.VRMS,SRC.MAINS.IRMS", headers=viewer).json()
        check(tr["tier"] == "fine" and len(tr["series"]) == 2 and len(tr["series"][0]["points"]) >= 1, "trend")
        check(c.get("/api/trend?tags=NO.SUCH.TAG", headers=viewer).status_code == 404, "unknown tag")
        ex = c.get("/api/export?tags=SY1.LOAD.VRMS", headers=viewer)
        check(ex.headers["content-type"].startswith("text/csv") and ex.text.splitlines()[0] == "ts,tag,value,unit,quality" and len(ex.text.splitlines()) >= 2, "CSV export")

        # ---- live stream: full snapshot first, then changes, @ 10 Hz
        with c.websocket_connect("/api/live?session=" + viewer["Authorization"][7:]) as ws:
            first = ws.receive_json()
            t0 = time.monotonic()
            n, sizes = 0, []
            while time.monotonic() - t0 < 3.0:
                msg = ws.receive_json()
                n += 1
                sizes.append(len(msg["tags"]))
        print(f"live stream: {n} messages in 3 s; first message {len(first['tags'])} tags, later {min(sizes)} to {max(sizes)}")
        check(len(first["tags"]) == len(snap["tags"]) and "auto" in first, "first stream message is the full snapshot")
        check(27 <= n <= 33, "stream at 10 Hz (CAP-M4-19)")
        check(max(sizes) < len(first["tags"]), "later messages hold changed values only")

        # ---- Operator: confirmation, command, acknowledgment, readback (Case 2)
        check(c.post("/api/commands/shed", headers=oper, json={"action": "contact", "bit": 1, "state": "open"}).status_code == 400, "confirmation required")
        check(c.post("/api/commands/shed", headers=oper, json={"action": "sideways", "confirm": True}).status_code == 422, "unknown action")
        r = c.post("/api/commands/shed", headers=oper, json={"action": "contact", "bit": 1, "state": "open", "confirm": True}).json()
        print(f"shed command: acknowledged in {r['ack_ms']} ms, result {r['result']}")
        check(r["result"] == "acknowledged" and r["ack_ms"] < 500 and r["reported_state"] == 2 and r["user"] == "oper1", f"command result {r}")
        card = c.get("/api/loads", headers=viewer).json()[0]
        check(card["contacts"][0] == {"bit": 1, "commanded": "open", "reported": "open"}, "load card shows commanded and reported state")
        r = c.post("/api/commands/shed", headers=oper, json={"action": "next", "confirm": True}).json()
        check(r["value"] == 0b11, "next opens the first closed contact of the sequence")
        r = c.post("/api/commands/shed", headers=oper, json={"action": "restore_last", "confirm": True}).json()
        check(r["value"] == 0b10, "restore_last closes the contact opened last")
        r = c.post("/api/commands/shed", headers=oper, json={"action": "contact", "bit": 1, "state": "closed", "confirm": True}).json()
        check(r["value"] == 0 and r["result"] == "acknowledged", "contact closed")
        check(c.post("/api/commands/shed", headers=oper, json={"action": "restore_last", "confirm": True}).json()["result"] == "no change", "nothing left to restore")

        # ---- energy reset (Case 10 step 3)
        r = c.post("/api/metering/SY1/reset", headers=oper, json={"confirm": True}).json()
        check(r["previous_value"] > 0, "previous energy returned")
        dt = wait_for(lambda: c.get("/api/metering", headers=viewer).json()[0]["e"] < r["previous_value"], 1.5)
        print(f"energy reads reset after {dt:.2f} s")
        check(dt is not None and dt <= 1.0 and c.get("/api/metering", headers=viewer).json()[0]["last_reset_ts"] is not None, "energy zero within 1 s")

        # ---- alarm acknowledgment
        cmd("offline 2")
        check(wait_for(lambda: any(a["class"] == "node offline" for a in c.get("/api/alarms", headers=viewer).json()), 5) is not None, "alarm listed")
        a = [a for a in c.get("/api/alarms", headers=viewer).json() if a["class"] == "node offline"][0]
        ack = c.post(f"/api/alarms/{a['alarm_id']}/ack", headers=oper, json={"confirm": True}).json()
        check(ack["acked_by"] == "oper1" and ack["acked_at"] is not None, "alarm acknowledged with a log entry")
        cmd("online 2")
        wait_for(lambda: not c.get("/api/alarms", headers=viewer).json(), 3)

        # ---- configuration: range check, previous value
        check(c.put("/api/config/i_s_max", headers=oper, json={"value": 9, "confirm": True}).status_code == 422, "out-of-range setting refused")
        check(c.put("/api/config/nope", headers=oper, json={"value": 1, "confirm": True}).status_code == 404, "unknown setting")
        for k, v in (("shed_dwell_s", 0.3), ("restore_margin_s", 0.6), ("restore_interval_s", 0.3)):
            c.put(f"/api/config/{k}", headers=oper, json={"value": v, "confirm": True})

        # ---- automatic shedding and restoration (Case 7)
        cmd("load medium")
        time.sleep(0.6)
        i_med = p.snapshot()[0]["SRC.MAINS.IRMS"][0]
        cmd("load high")
        time.sleep(0.6)
        i_high = p.snapshot()[0]["SRC.MAINS.IRMS"][0]
        i_s_max = (i_med + i_high) / 2 / 0.80  # limit midway between MEDIUM and HIGH
        r = c.put("/api/config/i_s_max", headers=oper, json={"value": i_s_max, "confirm": True}).json()
        check(r["previous"] == 2.9, "setting change returns the previous value")
        check(c.put("/api/control/auto", headers=oper, json={"shedding": True, "frequency": True, "confirm": True}).status_code == 422, "frequency response withdrawn")
        check(c.put("/api/control/auto", headers=oper, json={"shedding": True, "confirm": True}).json()["shedding"] is True, "automatic shedding enabled")
        dt = wait_for(lambda: p.demand.state == "exhausted", 6)
        con = sqlite3.connect(db)
        auto = con.execute("SELECT value, issued_at, acked_at, result FROM command WHERE origin='automatic' ORDER BY command_id").fetchall()
        gaps = [(b[1] - a[1]) / 1000 for a, b in zip(auto, auto[1:])]
        print(f"automatic shedding: {len(auto)} steps in {dt:.2f} s, values {[hex(v[0]) for v in auto]}, shortest gap {min(gaps):.2f} s")
        check([v[0] for v in auto] == [1, 3, 7, 15, 31, 63, 127], "contacts opened in sequence order; operator-only contact 7 untouched")
        check(all(v[2] is not None and v[2] - v[1] < 500 for v in auto), "every step acknowledged within 500 ms")
        check(min(gaps) >= 0.29, "no more than one step per dwell time")
        st = c.get("/api/snapshot", headers=viewer).json()["auto"]
        check(st["state"] == "exhausted" and [o["bit"] for o in st["opened"]] == list(range(7)) and st["current_a"] > st["limit_a"], "status reported to the HMI")
        check(p.snapshot()[0]["LCU.STAT.CONTACTS"][0] == 0x7F, "stand-in reports each contact open")
        cmd("load medium")
        dt = wait_for(lambda: p.commanded() == 0 and p.demand.state == "idle", 8)
        auto2 = con.execute("SELECT value FROM command WHERE origin='automatic' ORDER BY command_id").fetchall()[7:]
        print(f"restoration complete {dt:.2f} s after the load fell")
        check([v[0] for v in auto2] == [63, 31, 15, 7, 3, 1, 0], "restored in reverse order")

        # controller is held while source current is stale
        n_before = con.execute("SELECT COUNT(*) FROM command").fetchone()[0]
        cmd("offline 1")
        time.sleep(0.5)
        cmd("load high")
        time.sleep(1.2)
        check(p.demand.state == "holding" and con.execute("SELECT COUNT(*) FROM command").fetchone()[0] == n_before, "no step on stale data")
        c.put("/api/control/auto", headers=oper, json={"shedding": False, "confirm": True})
        cmd("online 1")
        cmd("load medium")
        time.sleep(0.5)
        check(p.commanded() == 0, "inhibited: no step after automatic control is switched off")

        # ---- session timeout (Case 10 step 4)
        c.put("/api/config/session_timeout_s", headers=oper, json={"value": 1, "confirm": True})
        time.sleep(1.3)
        check(c.post("/api/commands/shed", headers=oper, json={"action": "next", "confirm": True}).status_code == 401, "idle session ended")
        _, admin = login("admin", "admin-pass-1")
        c.cookies.clear()
        c.put("/api/config/session_timeout_s", headers=admin, json={"value": 900, "confirm": True})

        # ---- lockout, last Administrator, shutdown
        check(login("viewer1", "viewer-pass")[0] == 200, "a good login clears the failure count")
        codes = [login("viewer1", "bad")[0] for _ in range(5)] + [login("viewer1", "viewer-pass")[0]]
        c.cookies.clear()
        check(codes == [401] * 5 + [423], f"five failures lock the account: {codes}")
        me = [u for u in c.get("/api/users", headers=admin).json() if u["name"] == "admin"][0]
        check(c.request("DELETE", f"/api/users/{me['user_id']}", headers=admin, json={"confirm": True}).status_code == 409 and
              c.put(f"/api/users/{me['user_id']}", headers=admin, json={"role": "Viewer", "confirm": True}).status_code == 409, "last Administrator protected")
        check(c.post("/api/shutdown", headers=admin, json={"confirm": True}).status_code == 400 and
              c.post("/api/shutdown", headers=admin, json={"confirm": "CONFIRM"}).status_code == 501, "shutdown needs the typed confirmation; path not yet defined")

        # ---- audit trail (CAP-M4-53)
        p.stop()
        p = None
        acts = dict(con.execute("SELECT action, COUNT(*) FROM audit GROUP BY action").fetchall())
        print("audit entries:", acts)
        need = ["login", "denied", "shed", "config", "energy reset", "alarm ack", "export", "user created", "automatic shedding", "shutdown"]
        check(all(acts.get(a, 0) >= 1 for a in need), "every control and configuration action is in the audit log")
        check(acts["denied"] >= 6 and con.execute("SELECT COUNT(*) FROM audit WHERE action='login' AND result='failed'").fetchone()[0] >= 6, "refusals and failed logins recorded")
        check(con.execute("SELECT COUNT(*) FROM audit WHERE action='shed' AND user_id='oper1'").fetchone()[0] >= 4, "who, what, and result for commands")
        check(con.execute("SELECT COUNT(*) FROM user WHERE pw_hash LIKE '%pass%'").fetchone()[0] == 0, "no plaintext password stored")
        con.close()
    finally:
        if p:
            p.stop()
        sim.stdin.write("quit\n")
        sim.stdin.flush()
        sim.wait(timeout=2)
    print(f"{checks} checks, {fails} failed")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
