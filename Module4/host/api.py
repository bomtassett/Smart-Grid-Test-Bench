"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     2G - Applications (interface to 3H)
File:      Supervisory API (ICD-3).

Purpose:   Provides a data interface between host and system users on M3's HMI screen.
           Users ("clients") send logins, commands, and changes with POST, PUT, and DELETE; host answers with GET.
           Includes user login, roles, live data streamimg, metering, trends, alarms, load-control commands, configuration, and exports.

Usage:     app = create_app(poller)

Notes:     Session: single logged-in client.
           Role: Viewer, Operator, or Administrator.
           No protection path access.
           All control and configuration calls require login, role, confirmation, and are written to AUDIT.
           Cryptographic security layer w/ PBKDF2-HMAC-SHA256, 16-byte salt, and 200k iterations.
"""

import asyncio
import csv
import hashlib
import hmac
import io
import os
import secrets
import sqlite3
import threading
import time
from datetime import datetime, timezone

from fastapi import Body, Depends, FastAPI, HTTPException, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.responses import PlainTextResponse

from historian import GOOD, QUALITY

ROLES = {"Viewer": 1, "Operator": 2, "Administrator": 3}
COOKIE = "sgtb_session"
MAX_FAILED, LOCK_MS = 5, 15 * 60 * 1000

def hash_password(password, salt=None):
    """return salted hash of password."""
    salt = salt or secrets.token_bytes(16)
    dk = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 200_000)
    return salt.hex() + "$" + dk.hex()

def check_password(password, stored):
    """return true when password matches stored hash."""
    salt, _ = stored.split("$")
    return hmac.compare_digest(hash_password(password, bytes.fromhex(salt)), stored)

class Store:
    """API's connection to historian file."""

    def __init__(self, path):
        """open connection."""
        self.db = sqlite3.connect(path, check_same_thread=False, timeout=5.0)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.Lock()

    def q(self, sql, *args):
        """run query; return rows."""
        with self.lock:
            return self.db.execute(sql, args).fetchall()

    def x(self, sql, *args):
        """run statement and commit it."""
        with self.lock:
            cur = self.db.execute(sql, args)
            self.db.commit()
            return cur

def create_app(poller, admin_password=None):
    """build API application for poller."""
    app = FastAPI(title="SGTB supervisory API", version="0.1")
    st = Store(poller.cfg["database"])  # database connection
    sessions = {}                       # sessions, by token
    slock = threading.Lock()            # lock sessions
    now_ms = poller.hist.now_ms         # host time (ms)

    def audit(user, action, target=None, old=None, new=None, result="ok"):
        """write audit entry."""
        st.x("INSERT INTO audit (user_id, ts, action, target, old_value, new_value, result) VALUES (?,?,?,?,?,?,?)",
             user, now_ms(), action, target, None if old is None else str(old), None if new is None else str(new), result)

    # first start: create Administrator so bench is never w/o one
    if not st.q("SELECT 1 FROM user"):
        pw = admin_password or os.environ.get("SGTB_ADMIN_PASSWORD") or secrets.token_urlsafe(9)
        st.x("INSERT INTO user (name, role, pw_hash) VALUES (?,?,?)", "admin", "Administrator", hash_password(pw))
        if not (admin_password or os.environ.get("SGTB_ADMIN_PASSWORD")):
            print(f"First start: user 'admin' created with password {pw} (change it through /api/users)")
        audit(None, "user created", "admin", None, "Administrator")
    app.state.sessions = sessions

    # ------------------------------------------------------------- sessions
    def session_of(token):
        """return session of token, or None."""
        timeout = poller.rt["session_timeout_s"]
        with slock:
            s = sessions.get(token)
            if s is None:
                return None
            if time.monotonic() - s["last"] > timeout:
                del sessions[token]
                return None
            s["last"] = time.monotonic()
            return s

    def token_of(request):
        """return session token of request."""
        auth = request.headers.get("authorization", "")
        return auth[7:] if auth.lower().startswith("bearer ") else request.cookies.get(COOKIE)

    def need(role):
        """return dependency that requires role."""
        def dep(request: Request):
            """check session and role of request; return session."""
            s = session_of(token_of(request))
            if s is None:
                raise HTTPException(401, "login required")
            if ROLES[s["role"]] < ROLES[role]:
                audit(s["user"], "denied", f"{request.method} {request.url.path}", None, None, f"needs {role}")
                raise HTTPException(403, f"{role} role required")
            return s
        return dep

    def confirmed(body, token=True):
        """check confirmation in request body."""
        if body.get("confirm") != token:
            raise HTTPException(400, "confirmation required")

    @app.post("/api/login")
    def login(response: Response, body: dict = Body(...)):
        """log user in."""
        name, pw = str(body.get("user", "")), str(body.get("password", ""))
        rows = st.q("SELECT * FROM user WHERE name=? AND active=1", name)
        ts = now_ms()
        if rows and rows[0]["locked_until"] and rows[0]["locked_until"] > ts:
            audit(name, "login", None, None, None, "locked")
            raise HTTPException(423, "login failed")
        if not rows or not check_password(pw, rows[0]["pw_hash"]):
            if rows:
                n = rows[0]["failed_count"] + 1
                st.x("UPDATE user SET failed_count=?, locked_until=? WHERE name=?",
                     0 if n >= MAX_FAILED else n, ts + LOCK_MS if n >= MAX_FAILED else None, name)
            audit(name, "login", None, None, None, "failed")
            raise HTTPException(401, "login failed")  # one message, no further detail
        st.x("UPDATE user SET failed_count=0, locked_until=NULL WHERE name=?", name)
        token = secrets.token_urlsafe(32)
        with slock:
            sessions[token] = {"user": name, "role": rows[0]["role"], "last": time.monotonic()}
        response.set_cookie(COOKIE, token, httponly=True, samesite="strict")
        audit(name, "login", None, None, rows[0]["role"])
        return {"role": rows[0]["role"], "session": token}

    @app.post("/api/logout")
    def logout(request: Request, s=Depends(need("Viewer"))):
        """log user out."""
        with slock:
            sessions.pop(token_of(request), None)
        audit(s["user"], "logout")
        return {}

    # ------------------------------------------------------------ live tier
    def tag_values(tags):
        """return live values in form sent to clients."""
        return [dict(name=n, value=v, unit=poller.units.get(n, ""), ts=ts, quality=QUALITY[q], last_good_ts=lg)
                for n, (v, ts, q, lg) in sorted(tags.items())]

    def alarm_rows(active_only=True):
        """return alarm rows."""
        where = "WHERE cleared_at IS NULL" if active_only else ""
        return [dict(r) for r in st.q(f"SELECT * FROM alarm {where} ORDER BY alarm_id DESC LIMIT 200")]

    def auto_status():
        """return status of automatic control."""
        with poller.lock:
            cur = poller._good(poller.demand.current_tag)
        return poller.demand.status(cur)

    def snapshot():
        """return full snapshot."""
        tags, _ = poller.snapshot()
        return dict(ts=now_ms(), tags=tag_values(tags), switches={},
                    source={"active": poller.cfg["alarms"]["source_active"]},
                    alarms=alarm_rows(), auto=auto_status())

    @app.get("/api/snapshot")
    def get_snapshot(s=Depends(need("Viewer"))):
        """return full snapshot."""
        return snapshot()

    @app.websocket("/api/live")
    async def live(ws: WebSocket):
        """stream live values to client."""
        token = ws.query_params.get("session") or ws.cookies.get(COOKIE)
        if session_of(token) is None:
            await ws.close(code=4401)
            return
        await ws.accept()
        last = {}
        try:
            while True:
                if session_of(token) is None:  # session is kept alive by stream itself (ICD-3_3.3)
                    await ws.close(code=4401)
                    return
                tags, active = poller.snapshot()
                changed = {n: t for n, t in tags.items() if last.get(n) != (t[0], t[2])}
                last = {n: (t[0], t[2]) for n, t in tags.items()}
                msg = dict(ts=now_ms(), tags=tag_values(changed), alarms=[list(a) for a in active])
                if len(changed) == len(tags):  # first message is full snapshot
                    msg["auto"] = auto_status()
                await ws.send_json(msg)
                await asyncio.sleep(poller.cfg["poll"]["period_s"])
        except WebSocketDisconnect:
            pass

    # ---------------------------------------------------- metering and loads
    def load_nodes():
        """return nodes that have load card."""
        return [n.split("-")[1] for n in poller.nodes if n.split("-")[1] in poller.cfg["loads"]["cards"]]

    @app.get("/api/balance")
    def balance(window: str = "live", s=Depends(need("Viewer"))):
        """return generation, consumption, and balance."""
        if window not in ("live", "1h", "24h"):
            raise HTTPException(422, "window must be live, 1h, or 24h")
        if window == "live":
            tags, _ = poller.snapshot()
            val = lambda n: tags[n][0] if n in tags and tags[n][2] == GOOD else None
        else:
            t0 = now_ms() - (3600_000 if window == "1h" else 86_400_000)
            avg = {r["name"]: r["a"] for r in st.q(
                "SELECT name, AVG(value) a FROM sample_1hz JOIN tag USING(tag_id) WHERE ts>=? AND quality=0 GROUP BY name", t0)}
            val = avg.get
        return dict(window=window,
                    gen_by_source={poller.cfg["alarms"]["source_active"]: val("SYS.GEN.P")},
                    cons_by_load={n: val(f"{n}.LOAD.P") for n in load_nodes()},
                    generation=val("SYS.GEN.P"), consumption=val("SYS.CONS.P"), balance=val("SYS.BAL.P"))

    @app.get("/api/metering")
    def metering(s=Depends(need("Viewer"))):
        """return metered values of every load."""
        tags, _ = poller.snapshot()
        out = []
        for n in load_nodes():
            r = st.q("SELECT MAX(ts) t FROM audit WHERE action='energy reset' AND target=? AND result='ok'", n)
            out.append(dict(load_id=n, name=poller.cfg["loads"]["cards"][n]["name"], p=tags[f"{n}.LOAD.P"][0],
                            pf=tags[f"{n}.LOAD.PF"][0], e=tags[f"{n}.LOAD.E"][0], quality=QUALITY[tags[f"{n}.LOAD.P"][2]],
                            last_reset_ts=r[0]["t"]))
        return out

    @app.post("/api/metering/{load_id}/reset")
    def energy_reset(load_id: str, body: dict = Body(...), s=Depends(need("Operator"))):
        """reset energy total of load."""
        confirmed(body)
        if load_id not in load_nodes():
            raise HTTPException(404, "no such load")
        node = f"NODE-{load_id}"
        tags, _ = poller.snapshot()
        previous = tags[f"{load_id}.LOAD.E"][0]
        point = [p for p in poller.T["tags"] if p["tag"] == f"{load_id}.LOAD.E"][0]
        index = (point["addr"] - poller.T["poll"][node]["energy_addr"]) // 2
        err = poller.node_write(poller.T["poll"][node]["slave"], 102, [1 << index]).get(timeout=2)
        audit(s["user"], "energy reset", load_id, previous, 0, err or "ok")
        if err:
            raise HTTPException(502, err)
        return dict(previous_value=previous, reset_ts=now_ms())

    @app.get("/api/loads")
    def loads(s=Depends(need("Viewer"))):
        """return load cards."""
        tags, active = poller.snapshot()
        L = poller.cfg["loads"]
        commanded = poller.commanded()
        reported = tags["LCU.STAT.CONTACTS"][0] if "LCU.STAT.CONTACTS" in tags else None
        cards = []
        for n in load_nodes():
            g = lambda q: tags[f"{n}.LOAD.{q}"][0]
            faults = [c for c, t in active if t == n]
            faults += [c for c, t in active if t == "LCU" and c in ("comm loss", "state mismatch")]
            cards.append(dict(load_id=n, name=L["cards"][n]["name"], v=g("VRMS"), i=g("IRMS"), pf=g("PF"), p=g("P"), s=g("S"),
                              i_cap=L["i_cap_a"], quality=QUALITY[tags[f"{n}.LOAD.VRMS"][2]],
                              contacts=[dict(bit=b, commanded="open" if commanded & (1 << b) else "closed",
                                             reported=None if reported is None else ("open" if int(reported) & (1 << b) else "closed"))
                                        for b in L["cards"][n]["contacts"]],
                              faults=faults))
        return cards

    # ------------------------------------------------------ trends, exports
    def tag_row(name):
        """return tag row of name."""
        r = st.q("SELECT tag_id, unit FROM tag WHERE name=?", name)
        if not r:
            raise HTTPException(404, f"no such tag: {name}")
        return r[0]

    @app.get("/api/trend")
    def trend(tags: str, t_from: int = 0, t_to: int = 0, s=Depends(need("Viewer"))):
        """return trend of given tags."""
        t_to = t_to or now_ms()
        t_from = t_from or t_to - 600_000
        fine = t_from >= now_ms() - 86_400_000
        series = []
        for name in [t for t in tags.split(",") if t]:
            tid = tag_row(name)["tag_id"]
            if fine:
                pts = [[r["ts"], r["value"], QUALITY[r["quality"]]] for r in
                       st.q("SELECT ts, value, quality FROM sample_1hz WHERE tag_id=? AND ts BETWEEN ? AND ? ORDER BY ts", tid, t_from, t_to)]
            else:
                pts = [[r["ts_min"], r["avg"], r["min"], r["max"]] for r in
                       st.q("SELECT ts_min, avg, min, max FROM trend_1min WHERE tag_id=? AND ts_min BETWEEN ? AND ? ORDER BY ts_min", tid, t_from, t_to)]
            series.append(dict(tag=name, points=pts))
        return dict(tier="fine" if fine else "coarse", series=series)

    @app.get("/api/export")
    def export(tags: str, t_from: int = 0, t_to: int = 0, s=Depends(need("Viewer"))):
        """export given tags as CSV."""
        t_to = t_to or now_ms()
        buf = io.StringIO()
        w = csv.writer(buf)
        w.writerow(["ts", "tag", "value", "unit", "quality"])
        n = 0
        names = [t for t in tags.split(",") if t]
        for name in names:
            row = tag_row(name)
            first = None
            fine = st.q("SELECT ts, value, quality FROM sample_1hz WHERE tag_id=? AND ts BETWEEN ? AND ? ORDER BY ts", row["tag_id"], t_from, t_to)
            first = fine[0]["ts"] if fine else t_to + 1
            coarse = st.q("SELECT ts_min ts, avg value, 0 quality FROM trend_1min WHERE tag_id=? AND ts_min BETWEEN ? AND ? ORDER BY ts_min", row["tag_id"], t_from, first - 1)
            for r in list(coarse) + list(fine):
                iso = datetime.fromtimestamp(r["ts"] / 1000, timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
                w.writerow([iso, name, r["value"], row["unit"] or "", QUALITY.get(r["quality"], r["quality"])])
                n += 1
        audit(s["user"], "export", ",".join(names), None, f"{t_from}..{t_to}", f"{n} rows")
        fname = f"{'_'.join(names)}_{t_from}_{t_to}.csv".replace("/", "-")
        return PlainTextResponse(buf.getvalue(), media_type="text/csv",
                                 headers={"Content-Disposition": f'attachment; filename="{fname}"'})

    @app.get("/api/events")
    def events(s=Depends(need("Viewer"))):
        """return fault events."""
        return []  # EVENT rows will arrive w/ trip capture and ICD-4 link

    # ---------------------------------------------------------------- alarms
    @app.get("/api/alarms")
    def alarms(history: bool = False, s=Depends(need("Viewer"))):
        """return alarms."""
        return alarm_rows(active_only=not history)

    @app.post("/api/alarms/{alarm_id}/ack")
    def alarm_ack(alarm_id: int, body: dict = Body(...), s=Depends(need("Operator"))):
        """acknowledge alarm."""
        confirmed(body)
        if not st.q("SELECT 1 FROM alarm WHERE alarm_id=?", alarm_id):
            raise HTTPException(404, "no such alarm")
        st.x("UPDATE alarm SET acked_at=?, acked_by=? WHERE alarm_id=? AND acked_at IS NULL", now_ms(), s["user"], alarm_id)
        audit(s["user"], "alarm ack", str(alarm_id))
        return dict(st.q("SELECT * FROM alarm WHERE alarm_id=?", alarm_id)[0])

    # -------------------------------------------------------------- commands
    @app.post("/api/commands/shed")
    def shed(body: dict = Body(...), s=Depends(need("Operator"))):
        """send shed command."""
        confirmed(body)
        try:
            reply = poller.demand.manual(body.get("action"), body.get("bit"), body.get("state"), s["user"])
        except ValueError as e:
            raise HTTPException(422, str(e))
        res = reply.get(timeout=3)
        result = res["result"]
        reported = None
        if res["acked_at"] is not None:
            # CommandResult: readback that confirms contacts is waited for (within 1 s, ICD-6)
            deadline = time.monotonic() + poller.cfg["lcu"]["confirm_s"] + 0.3
            while time.monotonic() < deadline:
                row = st.q("SELECT result, reported_state FROM command WHERE command_id=?", res["command_id"])
                if row and row[0]["result"] in ("acknowledged", "mismatch"):
                    result, reported = row[0]["result"], row[0]["reported_state"]
                    break
                time.sleep(0.02)
        return dict(command_id=res["command_id"], issued_at=res["issued_at"], origin="operator", user=s["user"],
                    target_module="M3", register="LCU.CTRL.SHED", value=res["value"], acked_at=res["acked_at"],
                    ack_ms=round(res["ack_s"] * 1000, 1), reported_state=reported, result=result)

    @app.put("/api/control/auto")
    def control_auto(body: dict = Body(...), s=Depends(need("Operator"))):
        """switch automatic control on or off."""
        confirmed(body)
        if body.get("frequency"):
            raise HTTPException(422, "frequency response is withdrawn from supervisory control (CAP-M4-39)")
        if not isinstance(body.get("shedding"), bool):
            raise HTTPException(422, "shedding must be true or false")
        old = poller.demand.enabled
        poller.demand.set_enabled(body["shedding"])
        audit(s["user"], "automatic shedding", "SYS.CTRL.AUTO", old, body["shedding"])
        return dict(shedding=body["shedding"], frequency=False, changed_at=now_ms())

    # --------------------------------------------------------- configuration
    @app.get("/api/config")
    def config_list(s=Depends(need("Viewer"))):
        """return settings."""
        return [dict(r) for r in st.q("SELECT * FROM config ORDER BY key")]

    @app.put("/api/config/{key}")
    def config_set(key: str, body: dict = Body(...), s=Depends(need("Operator"))):
        """change one setting."""
        confirmed(body)
        row = st.q("SELECT * FROM config WHERE key=?", key)
        if not row:
            raise HTTPException(404, "no such setting")
        try:
            value = float(body.get("value"))
        except (TypeError, ValueError):
            raise HTTPException(422, "value must be a number")
        if not row[0]["min"] <= value <= row[0]["max"]:
            audit(s["user"], "config", key, row[0]["value"], value, "out of range")
            raise HTTPException(422, f"{key} must be within {row[0]['min']} to {row[0]['max']}")
        st.x("UPDATE config SET value=?, changed_at=?, changed_by=? WHERE key=?", value, now_ms(), s["user"], key)
        poller.rt[key] = value
        audit(s["user"], "config", key, row[0]["value"], value)
        out = dict(st.q("SELECT * FROM config WHERE key=?", key)[0])
        out["previous"] = row[0]["value"]
        return out

    # ------------------------------------------------------------------ users
    def user_public(r):
        """return public fields of user row."""
        return dict(user_id=r["user_id"], name=r["name"], role=r["role"], active=bool(r["active"]), locked_until=r["locked_until"])

    def admins(excluding=None):
        """return active Administrators, w/o one excluded."""
        return [r for r in st.q("SELECT * FROM user WHERE role='Administrator' AND active=1") if r["user_id"] != excluding]

    @app.get("/api/users")
    def users(s=Depends(need("Administrator"))):
        """return users."""
        return [user_public(r) for r in st.q("SELECT * FROM user ORDER BY user_id")]

    @app.post("/api/users")
    def user_add(body: dict = Body(...), s=Depends(need("Administrator"))):
        """add user."""
        confirmed(body)
        name, role, pw = body.get("name"), body.get("role"), body.get("password")
        if not name or role not in ROLES or not pw or len(pw) < 8:
            raise HTTPException(422, "name, a role, and a password of 8 or more characters are required")
        if st.q("SELECT 1 FROM user WHERE name=?", name):
            raise HTTPException(409, "name in use")
        st.x("INSERT INTO user (name, role, pw_hash) VALUES (?,?,?)", name, role, hash_password(pw))
        audit(s["user"], "user created", name, None, role)
        return user_public(st.q("SELECT * FROM user WHERE name=?", name)[0])

    @app.put("/api/users/{user_id}")
    def user_change(user_id: int, body: dict = Body(...), s=Depends(need("Administrator"))):
        """change user."""
        confirmed(body)
        row = st.q("SELECT * FROM user WHERE user_id=?", user_id)
        if not row:
            raise HTTPException(404, "no such user")
        role = body.get("role", row[0]["role"])
        active = int(body.get("active", row[0]["active"]))
        if role not in ROLES:
            raise HTTPException(422, "unknown role")
        if row[0]["role"] == "Administrator" and (role != "Administrator" or not active) and not admins(excluding=user_id):
            raise HTTPException(409, "the last Administrator cannot be removed or demoted")
        st.x("UPDATE user SET role=?, active=? WHERE user_id=?", role, active, user_id)
        if body.get("password"):
            if len(body["password"]) < 8:
                raise HTTPException(422, "password needs 8 or more characters")
            st.x("UPDATE user SET pw_hash=?, failed_count=0, locked_until=NULL WHERE user_id=?", hash_password(body["password"]), user_id)
        with slock:  # changed account must log in again
            for t in [t for t, v in sessions.items() if v["user"] == row[0]["name"]]:
                del sessions[t]
        audit(s["user"], "user changed", row[0]["name"], row[0]["role"], role)
        return user_public(st.q("SELECT * FROM user WHERE user_id=?", user_id)[0])

    @app.delete("/api/users/{user_id}")
    def user_delete(user_id: int, body: dict = Body(...), s=Depends(need("Administrator"))):
        """delete user."""
        confirmed(body)
        row = st.q("SELECT * FROM user WHERE user_id=?", user_id)
        if not row:
            raise HTTPException(404, "no such user")
        if row[0]["role"] == "Administrator" and not admins(excluding=user_id):
            raise HTTPException(409, "the last Administrator cannot be removed or demoted")
        st.x("DELETE FROM user WHERE user_id=?", user_id)
        with slock:
            for t in [t for t, v in sessions.items() if v["user"] == row[0]["name"]]:
                del sessions[t]
        audit(s["user"], "user deleted", row[0]["name"], row[0]["role"], None)
        return {}

    # --------------------------------------------------------------- shutdown
    @app.post("/api/shutdown")
    def shutdown(body: dict = Body(...), s=Depends(need("Operator"))):
        """stop host service."""
        confirmed(body, "CONFIRM")
        audit(s["user"], "shutdown", "SYS.CTRL.SHUTDOWN", None, None, "not implemented: path undefined (CAP-M4-41)")
        raise HTTPException(501, "the shutdown path is not yet defined with Module 1 (CAP-M4-41)")

    return app
