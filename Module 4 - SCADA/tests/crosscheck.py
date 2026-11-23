#!/usr/bin/env python3
"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     1D - Controller Network (test)
File:      Register Map Cross-check.

Purpose:   Proves that four descriptions of register map agree: YAML source, generated
           C header, generated tag table, and C register bank. Every tag of generated/tags.json
           is read from C slave (tests/mbsim) by independent Python master, and each decoded value
           is compared w/ value simulator was loaded with.

Usage:     python3 tests/crosscheck.py   (run by make test)
"""

import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent                   # package root
T = json.loads((ROOT / "generated" / "tags.json").read_text())  # tag table
_N = __import__("yaml").safe_load((ROOT / "regmap" / "regmap.yaml").read_text())["node"]  # node section of register map
POINT_BASE, POINT_BLOCK, SLOW_ADDR, ENERGY_ADDR = _N["point_base"], _N["point_block"], _N["slow_base"], _N["energy_base"]
WORDS = {"u16": 1, "i16": 1, "u32": 2, "i32": 2, "f32": 2}               # regs per data type
FMT = {"u16": ">H", "i16": ">h", "u32": ">I", "i32": ">i", "f32": ">f"}  # struct format per data type

def crc16(data: bytes) -> int:
    """return CRC-16 of block of bytes."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

def adu(payload: bytes) -> bytes:
    """return frame: payload w/ its CRC."""
    return payload + struct.pack("<H", crc16(payload))

class Slave:
    """C slave (tests/mbsim), run as child process."""
    def __init__(self, *args):
        """start slave."""
        self.p = subprocess.Popen([str(ROOT / "build" / "mbsim"), *args], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)

    def txn(self, frame: bytes):
        """send frame; return reply, or None."""
        self.p.stdin.write(frame.hex(" ") + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline().strip()
        if line == "-":
            return None
        rsp = bytes.fromhex(line)
        assert crc16(rsp) == 0, "bad CRC in response"
        return rsp

    def read(self, slave, table, addr, count):
        """read regs; return raw register bytes."""
        fc = 4 if table == "IR" else 3
        rsp = self.txn(adu(struct.pack(">BBHH", slave, fc, addr, count)))
        assert rsp is not None and rsp[1] == fc, f"read {table} {addr} failed: {rsp.hex(' ') if rsp else None}"
        return rsp[3:3 + 2 * count]

    def close(self):
        """stop slave."""
        self.p.stdin.close()
        self.p.wait()

def expected(tag):
    """return values loaded by tests/mbsim.c, by point index and quantity."""
    _, point, qty = tag["tag"].split(".")
    if point == "HLTH":
        return {"STATUS": 3, "TIME": 123456, "FWVER": 1}.get(qty)
    p = (tag["addr"] - POINT_BASE) // POINT_BLOCK if tag["addr"] < SLOW_ADDR else (tag["addr"] - ENERGY_ADDR) // 2
    return {"VRMS": 12.0 + p, "VDC": 12.0 + p, "IRMS": 0.1 * (p + 1), "IDC": 0.1 * (p + 1),
            "FREQ": 60.0, "ANG": -1.0 * p, "PF": 0.99, "P": 1.5 * (p + 1), "Q": -0.25 * p,
            "S": 1.6 * (p + 1), "E": 1.0 * (p + 1)}[qty]

def main():
    """run cross-check."""
    bad = n = 0
    nodes = {}
    for t in T["tags"]:
        if t["segment"] == "seg1":
            nodes.setdefault((t["node"], t["slave"]), []).append(t)
    for (node, slave), tags in sorted(nodes.items(), key=lambda x: x[0][1]):
        plan = T["poll"][node]
        npoints = (plan["slow_count"] - (plan["energy_addr"] - plan["slow_addr"])) // 2
        s = Slave("node", str(slave), str(npoints))
        # poller's two reads, made exactly as they are described in tags.json
        fast = s.read(slave, "IR", 0, T["poll"][node]["fast_count"])
        slow = s.read(slave, "IR", plan["slow_addr"], plan["slow_count"])
        for t in tags:
            w = WORDS[t["type"]]
            if t["addr"] >= plan["slow_addr"]:
                raw = slow[2 * (t["addr"] - plan["slow_addr"]):][:2 * w]
            else:
                raw = fast[2 * t["addr"]:][:2 * w]
            value = struct.unpack(FMT[t["type"]], raw)[0] * t["scale"]
            # every tag can also be read alone, by its published address (CAP-M4-35)
            assert s.read(slave, "IR", t["addr"], w) == raw, t["tag"]
            exp = expected(t)
            n += 1
            # float32 reg holds value itself; scaled integer is within half count
            tol = 1e-6 * max(1.0, abs(exp or 0.0)) if t["type"] == "f32" else 0.51 * t["scale"]
            if exp is not None and abs(value - exp) > tol:
                bad += 1
                print(f"MISMATCH {t['tag']}: read {value}, expected {exp}")
        # reg after last block is unmapped
        r = s.txn(adu(struct.pack(">BBHH", slave, 4, T["poll"][node]["fast_count"], 1)))
        assert r[1] == 0x84 and r[2] == 2
        s.close()

    # LCU: every shed bit is written and read back (ICD-6 conformance)
    lcu = {t["tag"]: t for t in T["tags"] if t["segment"] == "seg2"}
    a = lcu["LCU.CTRL.SHED"]["slave"]
    s = Slave("lcu")
    for bit in range(8):
        req = adu(struct.pack(">BBHH", a, 6, lcu["LCU.CTRL.SHED"]["addr"], 1 << bit))
        assert s.txn(req) == req, f"shed bit {bit} not acknowledged"
        got = struct.unpack(">HHH", s.read(a, "IR", lcu["LCU.STAT.CONTACTS"]["addr"], 3))
        n += 1
        if got[0] != 1 << bit or got[1] != 0:
            bad += 1
            print(f"MISMATCH LCU bit {bit}: contacts {got[0]:#06x}, fault {got[1]:#06x}")
    s.close()
    print(f"{n} tag and contact checks, {bad} mismatched")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
