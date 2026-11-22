#!/usr/bin/env python3
"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     2F/2G - Communication Servers / Applications
File:      Host Service Launcher.

Purpose:   Starts whole host service w/ one command: poller, historian, and supervisory API.

Usage:     python3 host/run_host.py [--config FILE] [--db FILE] [--no-api]

Notes:     @ first start user 'admin' is created and its password is printed once
           (unless SGTB_ADMIN_PASSWORD is set).
"""

import argparse
import threading
import time

from poller import Poller

def status_loop(p, stop):
    """print one status line per second."""
    while not stop.wait(1.0):
        tags, alarms = p.snapshot()
        s = p.stats

        def g(name):
            """return live value of tag, as text."""
            v = tags.get(name, (None,))[0]
            return "   --  " if v is None else f"{v:7.3f}"

        print(f"cycles {s['cycles']:6d}  overruns {s['overruns']}  bus {1e3 * s['bus_s'] / max(1, s['cycles']):4.1f} ms  "
              f"GEN {g('SYS.GEN.P')} W  CONS {g('SYS.CONS.P')} W  SY1 drop {g('SY1.LINE.VDROP')} V  "
              f"shed {p.commanded():#04x} ({p.demand.state})  alarms {['/'.join(a) for a in alarms]}")

def main():
    """start host service and run it until stopped."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--config")
    ap.add_argument("--db")
    ap.add_argument("--no-api", action="store_true", help="poller and historian only")
    ap.add_argument("--quiet", action="store_true", help="no status line each second")
    args = ap.parse_args()
    over = {"database": args.db} if args.db else {}
    p = Poller(args.config, **over)
    p.start()
    stop = threading.Event()
    if not args.quiet:
        threading.Thread(target=status_loop, args=(p, stop), daemon=True).start()
    try:
        if args.no_api:
            while True:
                time.sleep(1.0)
        else:
            import uvicorn
            from api import create_app
            uvicorn.run(create_app(p), host=p.cfg["api"]["bind"], port=p.cfg["api"]["port"], log_level="warning")
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        p.stop()

if __name__ == "__main__":
    main()
