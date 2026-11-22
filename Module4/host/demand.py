"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     2G - Applications
File:      Demand Controller: Supervisory Load Shedding (CAP-M4-38, ICD-6_6.5).

Purpose:   Sheds load automatically when source is overloaded, and restores it once overload
           has passed.

Notes:     Limit: source bus current above which load is shed.
           Sequence: order in which contacts are opened.
           Dwell: wait after each step. Restore margin: time below limit before restoration.
           Operator commands pass through same object as automatic steps.
           While data is stale, nothing is shed or restored.
"""

import threading

class DemandController:
    """demand controller."""
    def __init__(self, poller):
        """prepare controller from poller's settings."""
        self.p = poller  # poller
        s = poller.cfg["shed"]
        self.current_tag = s["current_tag"]                    # tag of source bus current
        self.sequence = list(s["sequence"])                    # automatic shed sequence (contact bits)
        self.operator_only = list(s.get("operator_only", []))  # contacts opened by operator only
        self.enabled = False                                   # true = automatic control is on
        self.opened = []                                       # contacts opened, in order: (bit, origin)
        self.state = "idle"                                    # controller state
        self._last_step = float("-inf")                        # time of last shed step
        self._below_since = None                               # time since which current has been below limit
        self._last_restore = float("-inf")                     # time of last restore step
        self._busy = False                                     # true = command is in flight
        self._lock = threading.Lock()                          # lock of `opened`

    # --------------------------------------------------------------- helpers
    def limit(self):
        """return current limit (A)."""
        return self.p.rt["shed_fraction"] * self.p.rt["i_s_max"]

    def _next_bit(self, commanded, manual):
        """return next contact to open, or None."""
        order = self.sequence + (self.operator_only if manual else [])
        for b in order:
            if not commanded & (1 << b):
                return b
        return None

    def _done(self, bit, opening, origin):
        """return callback that records step once its command has been answered."""
        def cb(res):
            """record step when command was acknowledged."""
            with self._lock:
                self._busy = False
                if res["acked_at"] is None:
                    return  # not acknowledged
                if opening:
                    if bit not in [b for b, _ in self.opened]:
                        self.opened.append((bit, origin))
                else:
                    self.opened = [(b, o) for b, o in self.opened if b != bit]
        return cb

    def status(self, current=None):
        """return status of controller."""
        with self._lock:
            return dict(shedding=self.enabled, state=self.state, limit_a=self.limit(), current_a=current,
                        opened=[dict(bit=b, origin=o) for b, o in self.opened])

    # ----------------------------------------------------- automatic control
    def set_enabled(self, on):
        """switch automatic control on or off."""
        with self._lock:
            self.enabled = bool(on)
            self._below_since = None
            self.state = "idle"

    def step(self, now):
        """run one control step; called by poller once per cycle."""
        if not self.enabled or self._busy:
            return
        rt = self.p.rt
        with self.p.lock:
            cur = self.p._good(self.current_tag)
        if cur is None:
            self._below_since = None
            self.state = "holding"
            return
        commanded = self.p.commanded()
        if cur > self.limit():
            self._below_since = None
            if now - self._last_step < rt["shed_dwell_s"]:
                return
            bit = self._next_bit(commanded, manual=False)
            if bit is None:
                self.state = "exhausted"  # sequence exhausted
                return
            self.state = "shedding"
            self._busy, self._last_step = True, now
            self.p.shed(lambda c, b=bit: c | (1 << b), origin="automatic", on_result=self._done(bit, True, "automatic"))
            return
        if self._below_since is None:
            self._below_since = now
        auto = [b for b, o in self.opened if o == "automatic"]
        if not auto:
            self.state = "idle"
            return
        self.state = "restoring"
        if (now - self._below_since >= rt["restore_margin_s"] and now - self._last_restore >= rt["restore_interval_s"]
                and now - self._last_step >= rt["shed_dwell_s"]):
            bit = auto[-1]
            self._busy, self._last_restore, self._last_step = True, now, now
            self.p.shed(lambda c, b=bit: c & ~(1 << b), origin="automatic", on_result=self._done(bit, False, "automatic"))

    # ------------------------------------------------------ operator control
    def manual(self, action, bit=None, state=None, user=None):
        """run operator command ('next', 'restore_last', or 'contact'); return its reply queue."""
        if action == "next":
            holder = {}

            def op(c):
                """return commanded bits w/ next contact opened."""
                b = self._next_bit(c, manual=True)
                holder["bit"] = b
                return None if b is None else c | (1 << b)

            def cb(res):
                """record step."""
                if holder.get("bit") is not None:
                    self._done(holder["bit"], True, "operator")(res)
            return self.p.shed(op, "operator", user, cb)
        if action == "restore_last":
            holder = {}

            def op(c):
                """return commanded bits w/ last opened contact closed."""
                with self._lock:
                    still = [b for b, _ in self.opened if c & (1 << b)]
                b = still[-1] if still else None
                holder["bit"] = b
                return None if b is None else c & ~(1 << b)

            def cb(res):
                """record step."""
                if holder.get("bit") is not None:
                    self._done(holder["bit"], False, "operator")(res)
            return self.p.shed(op, "operator", user, cb)
        if action == "contact":
            if bit is None or not 0 <= int(bit) <= 7 or state not in ("open", "closed"):
                raise ValueError("contact needs bit 0 to 7 and state 'open' or 'closed'")
            bit, opening = int(bit), state == "open"
            return self.p.shed(lambda c: (c | (1 << bit)) if opening else (c & ~(1 << bit)), "operator", user,
                               self._done(bit, opening, "operator"))
        raise ValueError("action must be next, restore_last, or contact")
