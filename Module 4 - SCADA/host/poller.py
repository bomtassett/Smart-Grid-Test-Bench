"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     2F/2G - Communication Servers / Applications
File:      Host Poller.

Purpose:   Core of host service. Every node and load control unit are polled; from the
           replies, live value of every tag is kept, derived quantities are computed, alarms are
           raised and cleared, load-control commands are sent, and data is passed to historian.

Notes:     Tag: one named value. Live tier: latest value of every tag, w/ its quality.
           Quality: good, stale, or invalid.
           Both serial ports and database are owned by one thread; other threads use snapshot() and shed().
           Firmware version of each node is compared w/ register map version;
             "firmware mismatch" alarm is raised while two differ.
"""

import queue
import struct
import threading
import time
from pathlib import Path

import yaml

from demand import DemandController
from historian import GOOD, INVALID, STALE, Historian
from sgtb_modbus import Master, ModbusError, NoReply, decode, load_tags

ANALOG_QTY = {"VRMS", "IRMS", "FREQ", "ANG", "PF", "P", "Q", "S", "E", "VDC", "IDC"}  # quantities stored as analog values
U32 = 1 << 32

class Live:
    """live value of one tag."""
    __slots__ = ("value", "ts", "quality", "last_good_ts")

    def __init__(self):
        """create empty live value."""
        self.value, self.ts, self.quality, self.last_good_ts = None, 0, INVALID, None

class Poller:
    """host poller."""
    def __init__(self, config_path=None, **override):
        """load settings and tag table, and open historian."""
        cfg_file = Path(config_path or Path(__file__).with_name("config.yaml"))
        self.cfg = yaml.safe_load(cfg_file.read_text())  # settings
        for key, value in override.items():              # e.g. ports={...}, database="..."
            if isinstance(value, dict):
                self.cfg[key].update(value)
            else:
                self.cfg[key] = value
        self.T = load_tags()  # tag table
        major, minor = (int(x) for x in self.T["version"].split("."))
        self.fw_expect = (major << 8) | minor        # node firmware version expected
        self.nodes = self.cfg["nodes"]               # nodes polled
        self.hist = Historian(self.cfg["database"])  # historian
        self.lock = threading.Lock()                 # lock of live tier
        self.live = {}                               # live tier, by tag
        self.alarms = {}                             # active alarms: (class, target) -> alarm id
        self._pending = {}                           # pending alarms: (class, target) -> start time
        self._cmdq = queue.Queue()                   # queued shed commands
        self._jobq = queue.Queue()                   # queued node writes
        self._read_energy_now = False                # true = energy is read in next cycle
        self.units = {}                              # units, by tag
        self._stop = threading.Event()               # stop request
        self._thread = None                          # poller thread
        self.stats = dict(cycles=0, overruns=0, bus_s=0.0, max_bus_s=0.0, lcu_ack_s=[])  # loop statistics

        self.node_tags = {n: [t for t in self.T["tags"] if t["node"] == n] for n in self.nodes}  # tags, by node
        self.lcu_tags = {t["tag"]: t for t in self.T["tags"] if t["segment"] == "seg2"}  # LCU tags, by name
        self._node_last_reply = {n: None for n in self.nodes}  # time of last reply, by node
        self._node_seq = {n: (None, 0.0) for n in self.nodes}  # last heartbeat and its time, by node
        self._lcu_missed = 0                  # LCU polls missed in a row
        self._lcu_commanded = None            # shed bits last commanded
        self._confirm = None                  # command awaiting confirmation: (id, value, deadline)
        self._state_last = {}                 # last state values, by tag
        self._trend = {}                      # trend sums, by tag
        self._trend_bucket = None             # trend minute in progress
        self._next_sample = time.monotonic()  # time of next 1 Hz sample

        self._register()
        self.rt = self.hist.seed_config({k: tuple(v) for k, v in self.cfg["shed"]["settings"].items()})  # runtime settings
        self.demand = DemandController(self)                                                             # demand controller
        self.m1 = Master(self.cfg["ports"]["seg1"], self.T["segments"]["seg1"]["baud"], self.cfg["poll"]["reply_timeout_s"])
        self.m2 = None
        if self.cfg["ports"].get("seg2"):
            self.m2 = Master(self.cfg["ports"]["seg2"], self.T["segments"]["seg2"]["baud"], 0.1)

    # ----------------------------------------------------------------- set-up
    def _derived_names(self):
        """return names of derived tags."""
        names = ["SYS.GEN.P", "SYS.CONS.P", "SYS.BAL.P", "SRC.XFMR.PLOSS"]
        for n in self.nodes:
            short = n.split("-")[1]
            if short.startswith("SY"):
                names += [f"{short}.LINE.VDROP", f"{short}.LINE.PLOSS"]
        return names

    def _register(self):
        """register every tag w/ historian and create live tier."""
        rows = []
        for n in self.nodes:
            for t in self.node_tags[n]:
                qty = t["tag"].rsplit(".", 1)[1]
                cls = "health" if ".HLTH." in t["tag"] else "analog"
                rows.append(dict(name=t["tag"], source="node", unit=t["unit"], data_type=t["type"], rate=t["rate_hz"],
                                 register_type=t["table"], address=t["addr"], node=n, mp=t["mp"], **{"class": cls}))
                self.live[t["tag"]] = Live()
                assert cls == "health" or qty in ANALOG_QTY
        for name in self._derived_names():
            unit = "V" if name.endswith("VDROP") else "W"
            rows.append(dict(name=name, source="host_computed", unit=unit, data_type="f32", rate=10,
                             register_type=None, address=None, node=None, mp=None, **{"class": "analog"}))
            self.live[name] = Live()
        for t in self.lcu_tags.values():
            rows.append(dict(name=t["tag"], source="m3_via_icd6", unit="", data_type=t["type"], rate=t["rate_hz"],
                             register_type=t["table"], address=t["addr"], node="LCU", mp=None, **{"class": "state"}))
            self.live[t["tag"]] = Live()
        self.hist.register_tags(rows)
        self.units = {r["name"]: r["unit"] for r in rows}

    # ------------------------------------------------------------- public API
    def start(self):
        """start poller thread."""
        self._thread = threading.Thread(target=self.run, name="poller", daemon=True)
        self._thread.start()

    def stop(self):
        """stop poller thread."""
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        self.hist.close()
        self.m1.close()
        if self.m2:
            self.m2.close()

    def snapshot(self):
        """return {tag: (value, ts, quality, last_good_ts)} and active alarms."""
        with self.lock:
            tags = {k: (v.value, v.ts, v.quality, v.last_good_ts) for k, v in self.live.items()}
            return tags, sorted(self.alarms)

    def shed(self, op, origin="operator", user=None, on_result=None):
        """queue shed command; return queue on which its result is delivered."""
        reply = queue.Queue(1)
        self._cmdq.put((op, origin, user, reply, on_result))
        return reply

    def node_write(self, slave, addr, values):
        """queue write of node holding regs; return queue on which its result is delivered."""
        reply = queue.Queue(1)
        self._jobq.put((slave, addr, list(values), reply))
        return reply

    def commanded(self):
        """return shed bits last commanded."""
        return self._lcu_commanded or 0

    # ------------------------------------------------------------------- loop
    def run(self):
        """poll loop."""
        period = self.cfg["poll"]["period_s"]
        nxt = time.monotonic()
        cycle = 0
        while not self._stop.is_set():
            bus = 0.0
            if cycle % 10 == 0:
                self._time_set()
            for n in self.nodes:
                bus += self._poll_node(n, energy=(cycle % 10 == 5) or self._read_energy_now)
            self._read_energy_now = False
            self._jobs()
            if self.m2:
                self._poll_lcu()
            with self.lock:
                self._derive()
            if self.m2:
                self.demand.step(time.monotonic())
                self._commands()
            with self.lock:
                self._evaluate_alarms()
            self._historize()
            self.hist.commit()
            self.stats["cycles"] += 1
            self.stats["bus_s"] += bus
            self.stats["max_bus_s"] = max(self.stats["max_bus_s"], bus)
            cycle += 1
            nxt += period
            delay = nxt - time.monotonic()
            if delay < 0:
                self.stats["overruns"] += 1
                nxt = time.monotonic()
            else:
                self._stop.wait(delay)

    # ---------------------------------------------------------------- segment 1
    def _time_set(self):
        """broadcast host time to every node."""
        ms = self.hist.now_ms() % U32
        self.m1.broadcast_write(100, [ms >> 16, ms & 0xFFFF])

    def _node_ts(self, node_ms, host_ms):
        """return time stamp (ms UTC) for node time."""
        diff = (host_ms - node_ms) % U32
        if diff > U32 // 2:
            diff -= U32
        return host_ms - diff if abs(diff) < 5000 else host_ms

    def _poll_node(self, node, energy):
        """poll one node; return bus time used (s)."""
        plan = self.T["poll"][node]
        short = node.split("-")[1]
        now = time.monotonic()
        host_ms = self.hist.now_ms()
        bus = 0.0
        try:
            fast = self.m1.read(plan["slave"], "IR", 0, plan["fast_count"])
            bus += self.m1.last_txn_s
            slow = None
            if energy:
                slow = self.m1.read(plan["slave"], "IR", plan["slow_addr"], plan["slow_count"])
                bus += self.m1.last_txn_s
        except ModbusError:
            bus += self.m1.last_txn_s
            return bus
        self._node_last_reply[node] = now
        vals = {}
        for t in self.node_tags[node]:
            if t["addr"] >= plan["slow_addr"]:
                if slow is not None:
                    vals[t["tag"]] = decode(t, slow, plan["slow_addr"])
            else:
                vals[t["tag"]] = decode(t, fast, 0)
        ts = self._node_ts(int(vals[f"{short}.HLTH.TIME"]), host_ms)
        seq = vals[f"{short}.HLTH.SEQ"]
        if seq != self._node_seq[node][0]:
            self._node_seq[node] = (seq, now)
        with self.lock:
            for name, v in vals.items():
                lv = self.live[name]
                lv.value, lv.ts, lv.quality, lv.last_good_ts = v, ts, GOOD, ts
        status = int(vals[f"{short}.HLTH.STATUS"])
        self._state(f"{short}.HLTH.STATUS", status, host_ms, ts)
        return bus

    def _jobs(self):
        """send queued node writes."""
        while not self._jobq.empty():
            slave, addr, values, reply = self._jobq.get()
            try:
                if len(values) == 1:
                    self.m1.write_single(slave, addr, values[0])
                else:
                    self.m1.write_multiple(slave, addr, values)
                self._read_energy_now = True  # energy read again at once
                reply.put(None)
            except ModbusError as e:
                reply.put(str(e))

    # ---------------------------------------------------------------- segment 2
    def _poll_lcu(self):
        """poll LCU."""
        a = self.lcu_tags["LCU.STAT.CONTACTS"]
        host_ms = self.hist.now_ms()
        try:
            c, f, s = struct.unpack(">HHH", self.m2.read(a["slave"], "IR", a["addr"], 3))
        except ModbusError:
            self._lcu_missed += 1
            return
        self._lcu_missed = 0
        with self.lock:
            for name, v in (("LCU.STAT.CONTACTS", c), ("LCU.STAT.FAULT", f), ("LCU.HLTH.SEQ", s)):
                lv = self.live[name]
                lv.value, lv.ts, lv.quality, lv.last_good_ts = v, host_ms, GOOD, host_ms
        self._state("LCU.STAT.CONTACTS", c, host_ms)
        self._state("LCU.STAT.FAULT", f, host_ms)
        if self._confirm:
            cid, value, deadline = self._confirm
            if c == value:
                self.hist.command_result(cid, c, "acknowledged")
                self._confirm = None
            elif time.monotonic() > deadline:
                self.hist.command_result(cid, c, "mismatch")
                self._confirm = None

    def _commands(self):
        """send queued shed commands."""
        lc = self.cfg["lcu"]
        shed = self.lcu_tags["LCU.CTRL.SHED"]
        while not self._cmdq.empty():
            op, origin, user, reply, on_result = self._cmdq.get()
            value = op(self.commanded()) if callable(op) else op
            if value is None:
                res = dict(command_id=None, issued_at=None, acked_at=None, ack_s=0.0, result="no change", value=self.commanded())
                if on_result:
                    on_result(res)
                reply.put(res)
                continue
            value = int(value) & 0xFFFF
            issued = self.hist.now_ms()
            t0 = time.monotonic()
            acked_at, result = None, "no reply"
            for attempt in range(2):  # ICD-6: one retry @ 250 ms, failure @ 500 ms
                self.m2.timeout = lc["ack_retry_s"] if attempt == 0 else lc["ack_fail_s"] - lc["ack_retry_s"]
                try:
                    self.m2.write_single(shed["slave"], shed["addr"], value)
                    acked_at, result = self.hist.now_ms(), "pending"
                    break
                except NoReply:
                    continue
                except ModbusError as e:
                    result = f"rejected: {e}"
                    break
            self.m2.timeout = 0.1
            ack_s = time.monotonic() - t0
            cid = self.hist.command(user_id=user, issued_at=issued, origin=origin, target_module="M3",
                                    register="LCU.CTRL.SHED", value=value, acked_at=acked_at,
                                    reported_state=None, result=result)
            self.hist.audit(user, "shed", "LCU.CTRL.SHED", str(self._lcu_commanded), str(value), result, ts=issued)
            if acked_at is not None:
                self._lcu_commanded = value
                self._confirm = (cid, value, time.monotonic() + lc["confirm_s"])
                self.stats["lcu_ack_s"].append(ack_s)
                with self.lock:
                    lv = self.live["LCU.CTRL.SHED"]
                    lv.value, lv.ts, lv.quality, lv.last_good_ts = value, acked_at, GOOD, acked_at
            res = dict(command_id=cid, issued_at=issued, acked_at=acked_at, ack_s=ack_s, result=result, value=value)
            if on_result:
                on_result(res)
            reply.put(res)

    # ------------------------------------------------------ derived and alarms
    def _good(self, name):
        """return value of tag when its quality is good, else None."""
        lv = self.live.get(name)
        return lv.value if lv is not None and lv.quality == GOOD else None

    def _set_derived(self, name, value, ts):
        """store derived value in live tier."""
        lv = self.live.get(name)
        if lv is None:
            return
        if value is None:
            if lv.quality == GOOD:
                lv.quality = STALE
        else:
            lv.value, lv.ts, lv.quality, lv.last_good_ts = value, ts, GOOD, ts

    def _derive(self):
        """compute derived quantities (ICD-2_2.4.1, CAP-M4-24)."""
        ts = self.hist.now_ms()
        d = self.cfg["derived"]
        gen = self._good(d["gen_tag"])
        loads = [self._good(f"{n.split('-')[1]}.LOAD.P") for n in self.nodes if n != "NODE-SRC"]
        cons = sum(loads) if loads and all(v is not None for v in loads) else None
        self._set_derived("SYS.GEN.P", gen, ts)
        self._set_derived("SYS.CONS.P", cons, ts)
        self._set_derived("SYS.BAL.P", gen - cons if gen is not None and cons is not None else None, ts)
        p3, p4 = self._good("SRC.SYNC.P"), self._good("SRC.SUBMAIN.P")
        self._set_derived("SRC.XFMR.PLOSS", p3 - p4 if p3 is not None and p4 is not None else None, ts)
        v4 = self._good("SRC.SUBMAIN.VRMS")
        for n in self.nodes:
            short = n.split("-")[1]
            if not short.startswith("SY"):
                continue
            v, i = self._good(f"{short}.SUB.VRMS"), self._good(f"{short}.SUB.IRMS")
            self._set_derived(f"{short}.LINE.VDROP", v4 - v if v4 is not None and v is not None else None, ts)
            self._set_derived(f"{short}.LINE.PLOSS", i * i * d["feeder_z_ohm"][short] if i is not None else None, ts)

    def _alarm(self, cls, target, active, text, now, ts):
        """raise or clear one alarm, after its delay."""
        key = (cls, target)
        if active:
            began = self._pending.setdefault(key, now)
            if key not in self.alarms and now - began >= self._alarm_delay(cls):
                self.alarms[key] = self.hist.alarm_raise(cls, target, text, ts)
        else:
            self._pending.pop(key, None)
            if key in self.alarms:
                self.hist.alarm_clear(self.alarms.pop(key), ts)

    def _alarm_delay(self, cls):
        """return delay of alarm class (s)."""
        # node loss and comm loss are already time-qualified by their own thresholds;
        # load rules have delays of CAP-M4-57
        fixed = {"node offline": 0.0, "comm loss": 0.0, "firmware mismatch": 0.0, "no feed": 0.0, "over cap": 1.0,
                 "load voltage out of band": 2.0, "state mismatch": 1.0}
        return fixed.get(cls, self.cfg["alarms"]["delay_s"])

    def _load_alarms(self, now, ts):
        """evaluate load alarms (CAP-M4-57)."""
        L = self.cfg["loads"]
        for n in self.nodes:
            short = n.split("-")[1]
            card = L["cards"].get(short)
            if card is None:
                continue
            v, i = self._good(f"{short}.LOAD.VRMS"), self._good(f"{short}.LOAD.IRMS")
            shed = any(self.commanded() & (1 << b) for b in card["contacts"])
            fed = v is not None and v >= L["v_no_feed"]
            self._alarm("over cap", short, fed and i is not None and i * 12.0 / v > L["i_cap_a"],
                        f"{i} A at {v} V exceeds {L['i_cap_a']} A scaled to 12 V", now, ts)
            self._alarm("load voltage out of band", short, fed and not L["v_low"] <= v <= L["v_high"],
                        f"{v} V outside {L['v_low']} to {L['v_high']} V", now, ts)
            self._alarm("no feed", short, v is not None and not fed and not shed, f"{v} V with contacts closed", now, ts)

    def _evaluate_alarms(self):
        """evaluate node and system alarms."""
        now, ts = time.monotonic(), self.hist.now_ms()
        a, p = self.cfg["alarms"], self.cfg["poll"]
        for n in self.nodes:
            short = n.split("-")[1]
            last = self._node_last_reply[n]
            silent = last is None or now - last > p["node_lost_s"]
            frozen = self._node_seq[n][0] is not None and now - self._node_seq[n][1] > p["node_lost_s"]
            lost = silent or frozen
            self._alarm("node offline", n, lost, "no reply" if silent else "heartbeat frozen", now, ts)
            stale_after = 3 * p["period_s"]
            for t in self.node_tags[n]:
                lv = self.live[t["tag"]]
                if lv.quality == GOOD and (last is None or now - last > stale_after) and t["rate_hz"] >= 10:
                    lv.quality = STALE  # CAP-M4-48: marked stale; last-good time is kept
                elif lv.quality == GOOD and lost:
                    lv.quality = STALE
            status = self._good(f"{short}.HLTH.STATUS")
            self._alarm("overrange", n, status is not None and bool(int(status) & 0x0004), "channel over range", now, ts)
            self._alarm("node fault", n, status is not None and bool(int(status) & 0x0008),
                        "a published value was not finite and reads 0", now, ts)
            fw = self._good(f"{short}.HLTH.FWVER")
            self._alarm("firmware mismatch", n, fw is not None and int(fw) != self.fw_expect,
                        f"node firmware {int(fw or 0) >> 8}.{int(fw or 0) & 0xFF} does not match host register map {self.T['version']}",
                        now, ts)
            lo = a["freq_nominal_hz"] * (1 - a["freq_band_pct"] / 100)
            hi = a["freq_nominal_hz"] * (1 + a["freq_band_pct"] / 100)
            for t in self.node_tags[n]:
                if t["tag"].endswith(".FREQ"):
                    f = self._good(t["tag"])
                    point = t["tag"].rsplit(".", 1)[0]
                    self._alarm("frequency out of band", point, f is not None and f > 0 and not lo <= f <= hi,
                                f"{f} Hz outside {lo:g} to {hi:g} Hz", now, ts)
        # OBJ 2.1.2: inverter output voltage; judged only while solar source is selected
        v = self._good(a["inverter_point"] + ".VRMS")
        band = a["inverter_v_nominal"] * a["inverter_band_pct"] / 100
        bad = a["source_active"] == "solar" and v is not None and abs(v - a["inverter_v_nominal"]) > band
        self._alarm("voltage out of band", a["inverter_point"], bad, f"{v} V outside {a['inverter_v_nominal']} V +/- {band:g} V", now, ts)
        self._load_alarms(now, ts)
        if self.m2:
            self._alarm("comm loss", "LCU", self._lcu_missed >= p["lcu_missed_polls"], "missed polls", now, ts)
            c, cmd = self._good("LCU.STAT.CONTACTS"), self._lcu_commanded
            self._alarm("state mismatch", "LCU", c is not None and cmd is not None and self._confirm is None and int(c) != cmd,
                        f"contacts {c} differ from command {cmd}", now, ts)
            if self._lcu_missed >= p["lcu_missed_polls"]:
                for name in ("LCU.STAT.CONTACTS", "LCU.STAT.FAULT", "LCU.HLTH.SEQ"):
                    if self.live[name].quality == GOOD:
                        self.live[name].quality = STALE

    # ---------------------------------------------------------------- historian
    def _state(self, name, value, ts, source_ts=None):
        """record state value when it has changed."""
        if self._state_last.get(name) != value:
            self._state_last[name] = value
            self.hist.state_change(name, ts, value, source_ts)

    def _analog(self):
        """yield live values that are stored as analog values."""
        for name, lv in self.live.items():
            if ".HLTH." in name or name.startswith("LCU."):
                continue
            yield name, lv

    def _historize(self):
        """pass samples and trend rows to historian when due."""
        h = self.cfg["historian"]
        now = time.monotonic()
        if now < self._next_sample:
            return
        self._next_sample += h["sample_period_s"]  # drift-free schedule
        if self._next_sample < now:
            self._next_sample = now + h["sample_period_s"]
        ts = self.hist.now_ms()
        with self.lock:
            rows = [(n, ts, lv.value, lv.quality) for n, lv in self._analog() if lv.value is not None]
        self.hist.write_samples(rows)
        bucket = ts // int(h["trend_period_s"] * 1000)
        if self._trend_bucket is not None and bucket != self._trend_bucket:
            ts_min = self._trend_bucket * int(h["trend_period_s"] * 1000)
            self.hist.write_trend([(n, ts_min, s / k if k else None, lo, hi, last, k)
                                   for n, (s, lo, hi, last, k) in self._trend.items()])
            self._trend = {}
            self.hist.prune(h["sample_retention_h"], h["retention_days"])
        self._trend_bucket = bucket
        for n, _, v, q in rows:
            acc = self._trend.setdefault(n, [0.0, None, None, v, 0])
            acc[3] = v
            if q == GOOD:
                acc[0] += v
                acc[1] = v if acc[1] is None else min(acc[1], v)
                acc[2] = v if acc[2] is None else max(acc[2], v)
                acc[4] += 1
        self.hist.commit()
