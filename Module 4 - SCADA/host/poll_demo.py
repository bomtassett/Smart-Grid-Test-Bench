#!/usr/bin/env python3
"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     2F - Communication Servers (bench tool)
File:      Live Tag Table, Bench Tool.

Purpose:   Shows live table of every value published by nodes, w/o historian or API.

Usage:     python3 host/poll_demo.py [PORT] [NODE ...]
           defaults: /tmp/sgtb-seg1 (sim/simnode), Demo 2 nodes.
"""

import sys
import time

from sgtb_modbus import Master, ModbusError, load_tags

QTY = ["VRMS", "IRMS", "FREQ", "ANG", "PF", "P", "Q", "S", "E"]  # quantities shown

def main():
    """poll nodes and print table until stopped."""
    port = sys.argv[1] if len(sys.argv) > 1 else "/tmp/sgtb-seg1"
    nodes = sys.argv[2:] or ["NODE-SRC", "NODE-SY1"]
    T = load_tags()
    m = Master(port, T["segments"]["seg1"]["baud"])
    cycle = 0
    try:
        while True:
            t0 = time.monotonic()
            lines, busy = [], 0.0
            for node in nodes:
                try:
                    v = poll_node_timed(m, T, node, cycle % 10 == 0)
                    busy += v.pop("_busy")
                except ModbusError as e:
                    lines.append(f"{node}: {e}")
                    continue
                name = node.split("-")[1]
                lines.append(f"{node}  status {int(v[name + '.HLTH.STATUS']):#06x}  seq {int(v[name + '.HLTH.SEQ'])}  time {int(v[name + '.HLTH.TIME'])} ms")
                points = sorted({k.split(".")[1] for k in v if ".HLTH." not in k})
                for p in points:
                    cells = []
                    for q in QTY:
                        key = f"{name}.{p}.{q}"
                        key = key if key in v else f"{name}.{p}.{ {'VRMS': 'VDC', 'IRMS': 'IDC'}.get(q, q) }"
                        if key in v:
                            last[key] = v[key]
                        if key in last:
                            cells.append(f"{q} {last[key]:9.4f}")
                    lines.append(f"  {p:8s} " + "  ".join(cells))
            print("\x1b[2J\x1b[H" + "\n".join(lines) + f"\n\nbus time this cycle {busy * 1e3:.1f} ms")
            cycle += 1
            time.sleep(max(0.0, 0.1 - (time.monotonic() - t0)))
    except KeyboardInterrupt:
        pass

last = {}  # values of previous poll, per node

def poll_node_timed(m, T, node, energy):
    """poll one node; return its values and bus time."""
    from sgtb_modbus import decode
    plan = T["poll"][node]
    busy = 0.0
    fast = m.read(plan["slave"], "IR", 0, plan["fast_count"])
    busy += m.last_txn_s
    slow = None
    if energy:
        slow = m.read(plan["slave"], "IR", plan["slow_addr"], plan["slow_count"])
        busy += m.last_txn_s
    out = {"_busy": busy}
    for t in T["tags"]:
        if t["node"] != node:
            continue
        if t["addr"] >= plan["slow_addr"]:
            if slow is not None:
                out[t["tag"]] = decode(t, slow, plan["slow_addr"])
        else:
            out[t["tag"]] = decode(t, fast, 0)
    return out

if __name__ == "__main__":
    main()
