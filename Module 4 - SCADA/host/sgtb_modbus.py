"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-14
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     2F - Communication Servers
File:      Host Modbus RTU Master & Tag Decoder.

Purpose:   Host's Modbus link; sends requests over serial port, checks replies,
           converts data to named values.

Notes:     Tag: single published value from node or load control unit.
           Addresses, types, and scales are taken from generated/tags.json.
"""

import json
import os
import select
import struct
import termios
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent                            # root
WORDS = {"u16": 1, "i16": 1, "u32": 2, "i32": 2, "f32": 2}               # regs per data type
FMT = {"u16": ">H", "i16": ">h", "u32": ">I", "i32": ">i", "f32": ">f"}  # struct format per data type

class ModbusError(Exception):
    """raise on Modbus error."""
    pass

class NoReply(ModbusError):
    """raise if slave unresponsive."""
    pass

def crc16(data: bytes) -> int:
    """return CRC-16 for block of bytes."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

def load_tags(path=None):
    """load tag table."""
    return json.loads(Path(path or ROOT / "generated" / "tags.json").read_text())

class Master:
    """Modbus RTU master; serial port."""
    def __init__(self, port, baud=115200, timeout=0.05):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)  # serial port
        self.timeout = timeout                                            # reply timeout (s)
        tio = termios.tcgetattr(self.fd)
        tio[0] = 0                                             # iflag: raw
        tio[1] = 0                                             # oflag: raw
        tio[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # 8N1
        tio[3] = 0                                             # lflag: raw
        speed = getattr(termios, f"B{baud}")
        tio[4] = tio[5] = speed
        termios.tcsetattr(self.fd, termios.TCSANOW, tio)
        self.char_s = 10.0 / baud  # duration, one char (s)
        self.last_txn_s = 0.0      # duration, last transaction (s)

    def close(self):
        os.close(self.fd)

    def _txn(self, payload: bytes, expect: int) -> bytes:
        """send request, return checked reply."""
        try:  # discard stale input
            while os.read(self.fd, 256):
                pass
        except BlockingIOError:
            pass
        t0 = time.monotonic()
        os.write(self.fd, payload + struct.pack("<H", crc16(payload)))
        buf = b""
        deadline = t0 + self.timeout
        while time.monotonic() < deadline:
            r, _, _ = select.select([self.fd], [], [], max(0.0, deadline - time.monotonic()))
            if not r:
                break
            buf += os.read(self.fd, 256)
            # exception response, or normal response
            if len(buf) >= 5 and buf[1] & 0x80:
                break
            if len(buf) >= expect:
                break
        self.last_txn_s = time.monotonic() - t0
        if not buf:
            raise NoReply(f"no reply from slave {payload[0]}")
        if crc16(buf) != 0:
            raise ModbusError(f"bad CRC from slave {payload[0]}: {buf.hex(' ')}")
        if buf[1] & 0x80:
            raise ModbusError(f"exception {buf[2]} from slave {payload[0]} (function {payload[1]})")
        return buf

    def read(self, slave, table, addr, count):
        """read `count` regs"""
        fc = 4 if table == "IR" else 3
        rsp = self._txn(struct.pack(">BBHH", slave, fc, addr, count), 5 + 2 * count)
        return rsp[3:3 + 2 * count]

    def write_single(self, slave, addr, value):
        """write to holding reg."""
        req = struct.pack(">BBHH", slave, 6, addr, value)
        if self._txn(req, 8)[:6] != req:
            raise ModbusError("write not echoed")

    def broadcast_write(self, addr, values):
        """broadcast write to holding regs (all slaves receive, no response)."""
        body = b"".join(struct.pack(">H", v) for v in values)
        frame = struct.pack(">BBHHB", 0, 16, addr, len(values), len(body)) + body
        os.write(self.fd, frame + struct.pack("<H", crc16(frame)))
        # hold bus for frame + t_silent
        time.sleep((len(frame) + 2 + 3.5) * self.char_s + 0.003)

    def write_multiple(self, slave, addr, values):
        body = b"".join(struct.pack(">H", v) for v in values)
        self._txn(struct.pack(">BBHHB", slave, 16, addr, len(values), len(body)) + body, 8)

def decode(tag, raw, base_addr):
    """return one tag from block of register bytes."""
    off = 2 * (tag["addr"] - base_addr)
    return struct.unpack(FMT[tag["type"]], raw[off:off + 2 * WORDS[tag["type"]]])[0] * tag["scale"]

def poll_node(m, T, node, energy=True):
    """poll node; return {tag: value}."""
    plan = T["poll"][node]
    fast = m.read(plan["slave"], "IR", 0, plan["fast_count"])
    slow = m.read(plan["slave"], "IR", plan["slow_addr"], plan["slow_count"]) if energy else None
    out = {}
    for t in T["tags"]:
        if t["node"] != node:
            continue
        if t["addr"] >= plan["slow_addr"]:
            if slow is not None:
                out[t["tag"]] = decode(t, slow, plan["slow_addr"])
        else:
            out[t["tag"]] = decode(t, fast, 0)
    return out
