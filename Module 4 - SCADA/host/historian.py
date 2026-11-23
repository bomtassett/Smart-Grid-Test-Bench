"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     2G - Applications
File:      SQLite Historian (ICD-5).

Purpose:   Long-term memory of host. Measured values, state changes, commands, alarms, and an
           audit trail are stored in one SQLite database file, from which they can be exported.

Notes:     WAL mode, one file, one writer.
           Table and column names are those of Table ICD-5.2, in lower case.
"""

import csv
import sqlite3
import threading
import time
from datetime import datetime, timezone

# database schema
SCHEMA = """
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
CREATE TABLE IF NOT EXISTS tag (
    tag_id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL, source TEXT, class TEXT,
    unit TEXT, data_type TEXT, rate REAL, register_type TEXT, address INTEGER, node TEXT, mp TEXT);
CREATE TABLE IF NOT EXISTS sample_1hz (
    tag_id INTEGER NOT NULL, ts INTEGER NOT NULL, value REAL, quality INTEGER NOT NULL,
    PRIMARY KEY (tag_id, ts)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS trend_1min (
    tag_id INTEGER NOT NULL, ts_min INTEGER NOT NULL, avg REAL, min REAL, max REAL, last REAL,
    n_good INTEGER NOT NULL, PRIMARY KEY (tag_id, ts_min)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS state_change (
    tag_id INTEGER NOT NULL, ts INTEGER NOT NULL, value INTEGER, source_ts INTEGER,
    PRIMARY KEY (tag_id, ts)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS command (
    command_id INTEGER PRIMARY KEY, user_id TEXT, issued_at INTEGER NOT NULL, origin TEXT NOT NULL,
    target_module TEXT, register TEXT, value INTEGER, acked_at INTEGER, reported_state INTEGER, result TEXT);
CREATE TABLE IF NOT EXISTS alarm (
    alarm_id INTEGER PRIMARY KEY, class TEXT NOT NULL, target TEXT NOT NULL, text TEXT,
    raised_at INTEGER NOT NULL, cleared_at INTEGER, acked_at INTEGER, acked_by TEXT);
CREATE TABLE IF NOT EXISTS audit (
    audit_id INTEGER PRIMARY KEY, user_id TEXT, ts INTEGER NOT NULL, action TEXT NOT NULL,
    target TEXT, old_value TEXT, new_value TEXT, result TEXT);
CREATE TABLE IF NOT EXISTS user (
    user_id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL, role TEXT NOT NULL, pw_hash TEXT NOT NULL,
    failed_count INTEGER NOT NULL DEFAULT 0, locked_until INTEGER, active INTEGER NOT NULL DEFAULT 1);
CREATE TABLE IF NOT EXISTS config (
    key TEXT PRIMARY KEY, value REAL NOT NULL, unit TEXT, min REAL, max REAL, "default" REAL,
    changed_at INTEGER, changed_by TEXT);
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
CREATE INDEX IF NOT EXISTS alarm_open ON alarm (cleared_at) WHERE cleared_at IS NULL;
"""

GOOD, STALE, INVALID = 0, 1, 2                                # quality codes
QUALITY = {GOOD: "good", STALE: "stale", INVALID: "invalid"}  # quality names

class Historian:
    """SQLite historian."""
    def __init__(self, path):
        """open database and create any missing table."""
        self.db = sqlite3.connect(path, check_same_thread=False)  # database connection
        self.db.executescript(SCHEMA)
        self.tag_id = dict(self.db.execute("SELECT name, tag_id FROM tag"))  # tag ids, by name
        self._clock = threading.Lock()                                       # lock of clock
        # time stamps never step backward across restart (CST-M4-16)
        row = self.db.execute("SELECT value FROM meta WHERE key='last_ts'").fetchone()
        self._last_ts = int(row[0]) if row else 0  # last time stamp issued (ms)
        self._offset = 0                           # correction added to host clock (ms)
        wall = int(time.time() * 1000)
        if wall <= self._last_ts:
            self._offset = self._last_ts + 1 - wall
            self.audit(None, "clock", "host", str(wall), str(self._last_ts + 1),
                       "backward step corrected", ts=self._last_ts + 1)
        self.db.commit()

    def now_ms(self):
        """return host time (ms UTC); never steps backward."""
        with self._clock:
            ts = int(time.time() * 1000) + self._offset
            if ts <= self._last_ts:
                ts = self._last_ts + 1
            self._last_ts = ts
            return ts

    # ------------------------------------------------------------ configuration
    def register_tags(self, tags):
        """add or update tags."""
        for t in tags:
            self.db.execute(
                "INSERT INTO tag (name, source, class, unit, data_type, rate, register_type, address, node, mp) "
                "VALUES (:name,:source,:class,:unit,:data_type,:rate,:register_type,:address,:node,:mp) "
                "ON CONFLICT(name) DO UPDATE SET source=excluded.source, class=excluded.class, unit=excluded.unit, "
                "data_type=excluded.data_type, rate=excluded.rate, register_type=excluded.register_type, "
                "address=excluded.address, node=excluded.node, mp=excluded.mp", t)
        self.db.commit()
        self.tag_id = dict(self.db.execute("SELECT name, tag_id FROM tag"))
        self._clock = threading.Lock()

    # ------------------------------------------------------------------ writers
    def write_samples(self, rows):
        """write 1 Hz samples."""
        self.db.executemany("INSERT OR REPLACE INTO sample_1hz VALUES (?,?,?,?)",
                            [(self.tag_id[n], ts, v, q) for n, ts, v, q in rows])

    def write_trend(self, rows):
        """write trend rows."""
        self.db.executemany("INSERT OR REPLACE INTO trend_1min VALUES (?,?,?,?,?,?,?)",
                            [(self.tag_id[r[0]],) + tuple(r[1:]) for r in rows])

    def state_change(self, name, ts, value, source_ts=None):
        """write one state change."""
        self.db.execute("INSERT OR REPLACE INTO state_change VALUES (?,?,?,?)",
                        (self.tag_id[name], ts, int(value), source_ts))

    def command(self, **c):
        """write one command; return its id."""
        cur = self.db.execute(
            "INSERT INTO command (user_id, issued_at, origin, target_module, register, value, acked_at, "
            "reported_state, result) VALUES (:user_id,:issued_at,:origin,:target_module,:register,:value,"
            ":acked_at,:reported_state,:result)", c)
        return cur.lastrowid

    def command_result(self, command_id, reported_state, result):
        """write result of command."""
        self.db.execute("UPDATE command SET reported_state=?, result=? WHERE command_id=?",
                        (reported_state, result, command_id))

    def alarm_raise(self, cls, target, text, ts):
        """write new alarm; return its id."""
        return self.db.execute("INSERT INTO alarm (class, target, text, raised_at) VALUES (?,?,?,?)",
                               (cls, target, text, ts)).lastrowid

    def alarm_clear(self, alarm_id, ts):
        """mark alarm as cleared."""
        self.db.execute("UPDATE alarm SET cleared_at=? WHERE alarm_id=?", (ts, alarm_id))

    def audit(self, user, action, target, old, new, result, ts=None):
        """write one audit entry."""
        self.db.execute("INSERT INTO audit (user_id, ts, action, target, old_value, new_value, result) "
                        "VALUES (?,?,?,?,?,?,?)", (user, ts or self.now_ms(), action, target, old, new, result))

    def seed_config(self, items):
        """add missing settings w/ their defaults; return {key: value}."""
        for key, (default, unit, lo, hi) in items.items():
            self.db.execute('INSERT OR IGNORE INTO config (key, value, unit, min, max, "default") VALUES (?,?,?,?,?,?)',
                            (key, default, unit, lo, hi, default))
        self.db.commit()
        return dict(self.db.execute("SELECT key, value FROM config"))

    def commit(self):
        """commit pending writes."""
        self.db.execute("INSERT OR REPLACE INTO meta VALUES ('last_ts', ?)", (str(self._last_ts),))
        self.db.commit()

    def prune(self, sample_retention_h, retention_days):
        """delete data older than retention times."""
        now = self.now_ms()
        self.db.execute("DELETE FROM sample_1hz WHERE ts < ?", (now - int(sample_retention_h * 3600e3),))
        old = now - int(retention_days * 86400e3)
        self.db.execute("DELETE FROM trend_1min WHERE ts_min < ?", (old,))
        for table, col in (("state_change", "ts"), ("command", "issued_at"), ("alarm", "raised_at"), ("audit", "ts")):
            self.db.execute(f"DELETE FROM {table} WHERE {col} < ?", (old,))

    # ------------------------------------------------------------------ exports
    def export_csv(self, tag, t_from, t_to, out_path, user=None):
        """export samples of one tag as CSV (ICD-5_5.4)."""
        tid = self.tag_id[tag]
        unit = self.db.execute("SELECT unit FROM tag WHERE tag_id=?", (tid,)).fetchone()[0] or ""
        rows = self.db.execute("SELECT ts, value, quality FROM sample_1hz WHERE tag_id=? AND ts BETWEEN ? AND ? "
                               "ORDER BY ts", (tid, t_from, t_to)).fetchall()
        first = rows[0][0] if rows else t_to + 1
        older = self.db.execute("SELECT ts_min, avg, 0 FROM trend_1min WHERE tag_id=? AND ts_min BETWEEN ? AND ? "
                                "ORDER BY ts_min", (tid, t_from, first - 1)).fetchall()
        with open(out_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["ts", "tag", "value", "unit", "quality"])
            for ts, value, q in older + rows:
                iso = datetime.fromtimestamp(ts / 1000, timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
                w.writerow([iso, tag, value, unit, QUALITY.get(q, q)])
        self.audit(user, "export", tag, None, f"{t_from}..{t_to}", f"{len(older) + len(rows)} rows")
        self.commit()
        return len(older) + len(rows)

    def snapshot(self, out_path, user=None):
        """copy whole database to one file (CAP-M4-58)."""
        self.audit(user, "snapshot", out_path, None, None, "started")
        self.commit()
        dst = sqlite3.connect(out_path)
        self.db.backup(dst)
        dst.close()

    def close(self):
        """close database."""
        self.commit()
        self.db.close()
