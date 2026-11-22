#!/usr/bin/env python3
"""
THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
Engineer:  Thomas Bassett
Created:   2026-11-20
Modified:  2026-11-22

Project:   Smart Grid Test Bench
Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
Level:     1C - Local Controllers (test)
File:      Emulated Test of the NUCLEO-G474RE Node Images.

Purpose:   Runs real firmware image on software model of processor (Unicorn, Cortex-M4) and of
           the few peripherals it uses, so that board firmware can be exercised before board exists.
           Executed: start-up, clock set-up, sampling irq (fed w/ synthetic converter data), ms tick,
           RS-485 irq (fed w/ Modbus frames), main loop, and flash store.
           Checked: peripheral regs programmed, console text, Modbus replies, and state
           stored across restart.

Usage:     python3 tests/emu_nucleo.py   (images it needs are built first)
           needs: pip install unicorn numpy; arm-none-eabi-gcc.

Notes:     Only register behaviour is modelled: no analog behaviour, no timing inside peripheral,
             and no irq pre-emption (irqs are delivered between main-loop passes).
           A PASS HERE IS NOT A PASS ON THE BOARD.
"""

import math, os, struct, subprocess, sys
import numpy as np
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, \
    UC_HOOK_MEM_UNMAPPED, UC_HOOK_CODE, UcError
from unicorn.arm_const import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # package root
BOARD = os.path.join(ROOT, "firmware", "board", "nucleo_g474re")    # board folder

FLASH, FLASH_SIZE = 0x08000000, 0x80000
RAM, RAM_SIZE = 0x20000000, 0x20000
STOP = 0x2001F000  # return address of injected handler call
NV_BASE, NV_END = 0x0807C000, 0x08080000

RCC, FLASHR, PWR = 0x40021000, 0x40022000, 0x40007000
USART3, LPUART1, TIM6 = 0x40004800, 0x40008000, 0x40001000
DMA1, DMAMUX1 = 0x40020000, 0x40020800
GPIOA, GPIOB, GPIOC = 0x48000000, 0x48000400, 0x48000800
ADC1, ADC2, ADC12C = 0x50000000, 0x50000100, 0x50000300

F_SYS = 170e6           # system clock (Hz)
F_SCAN = F_SYS / 2767   # converter triggers per second
T_CONV = 60.0 / 42.5e6  # conversion time (s)

def crc16(data):
    """return CRC-16 of block of bytes."""
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c

class Signal:
    """one measurement point @ line level, and its two converter pins."""
    def __init__(self, v_on_master, k_div, r_shunt, vrms, irms, lag_deg=0.0, f=60.0, dc=False, mid=1.65):
        """set signal of one point."""
        self.__dict__.update(locals())

    def codes(self, t):
        """return converter codes of pair @ time t."""
        if self.dc:
            v = np.full_like(t, self.vrms)
            i = np.full_like(t, self.irms)
        else:
            w = 2 * math.pi * self.f * t
            v = self.vrms * math.sqrt(2) * np.sin(w)
            i = self.irms * math.sqrt(2) * np.sin(w - math.radians(self.lag_deg))
        pin_v = self.mid + v * self.k_div
        pin_i = self.mid + i * self.r_shunt * 20.0
        return pin_v, pin_i

class Node:
    """one emulated node."""
    def __init__(self, node, signals, extra="", flash=None, seed=1):
        """build image if needed, and load it into emulator."""
        self.node, self.signals = node, signals
        out = os.path.join("build", "emu_" + node + ("_fast" if extra else ""))
        subprocess.run(["make", "-s", "-C", BOARD, f"NODE={node}", f"OUT={out}", f"EXTRA={extra}", "all"],
                       check=True, stdout=subprocess.DEVNULL)
        elf = os.path.join(BOARD, out, f"m4node_{node}.elf")
        self.sym = {}
        for line in subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True, check=True).stdout.splitlines():
            a, _, n = line.split()
            self.sym[n] = int(a, 16)
        image = open(elf[:-4] + ".bin", "rb").read()
        self.flash = bytearray(flash) if flash else bytearray(b"\xFF" * FLASH_SIZE)
        self.flash[:len(image)] = image
        self.image_len = len(image)

        self.rng = np.random.default_rng(seed)
        self.console, self.tx, self.problems = bytearray(), bytearray(), []
        self.pending, self.sh = [], {}
        self.programs = self.erases = 0
        self.flash_key = 0
        self.t_samples = 0  # output samples delivered
        self.ms = 0
        self.half = 0
        self.led = self.rate_pin = 0

        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M4)
        uc.mem_map(FLASH, FLASH_SIZE)
        uc.mem_write(FLASH, bytes(self.flash))
        uc.mem_map(RAM, RAM_SIZE)
        uc.mem_map(0x40000000, 0x30000)
        uc.mem_map(0x48000000, 0x2000)
        uc.mem_map(0x50000000, 0x1000)
        uc.mem_map(0xE0000000, 0x100000)
        # reset values on which fw depends
        for a, v in [(RCC, 0x63), (FLASHR, 0x00040601), (FLASHR + 0x14, 0xC0000000), (FLASHR + 0x20, 0xFFEFF8AA),
                     (ADC1 + 8, 0x20000000), (ADC2 + 8, 0x20000000), (USART3 + 0x1C, 0xC0), (LPUART1 + 0x1C, 0xC0),
                     (PWR + 0x80, 0x100), (GPIOA, 0xABFFFFFF), (GPIOB, 0xFFFFFEBF), (GPIOC, 0xFFFFFFFF)]:
            self.w32(a, v)
        self.sh = {FLASHR + 0x10: 0, FLASHR + 0x14: 0xC0000000, USART3 + 0x1C: 0xC0, DMA1: 0, ADC1: 0, ADC2: 0}
        for lo, hi in [(0x40000000, 0x4002FFFF), (0x48000000, 0x48001FFF), (0x50000000, 0x50000FFF)]:
            uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write, begin=lo, end=hi)
            uc.hook_add(UC_HOOK_MEM_READ, self.on_read, begin=lo, end=hi)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_flash_write, begin=FLASH, end=FLASH + FLASH_SIZE - 1)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.on_unmapped)
        # FPU is switched on, as SystemInit does through coprocessor access register
        uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        sp, pc = struct.unpack("<II", self.flash[:8])
        uc.reg_write(UC_ARM_REG_SP, sp)
        self.pc = pc
        self.loop = self.sym["rs485_frame"] & ~1
        self.skip = self.at_loop = False
        uc.hook_add(UC_HOOK_CODE, self.on_loop, begin=self.loop, end=self.loop)
        self.run_main()

    # ------------------------------------------------------------ memory helpers
    def r32(self, a): return struct.unpack("<I", self.uc.mem_read(a, 4))[0]       # read 32-bit word
    def w32(self, a, v): self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))  # write 32-bit word
    def var(self, name, fmt="<I"): return struct.unpack(fmt, self.uc.mem_read(self.sym[name], struct.calcsize(fmt)))[0]  # read firmware variable

    def on_unmapped(self, uc, access, addr, size, value, _):
        """report access to unmapped memory."""
        self.problems.append(f"unmapped access at 0x{addr:08X}, pc 0x{uc.reg_read(UC_ARM_REG_PC):08X}")
        return False

    def on_flash_write(self, uc, access, addr, size, value, _):
        """model write to flash."""
        self.flush()
        cr = self.sh[FLASHR + 0x14]
        if not (NV_BASE <= addr < NV_END):
            self.problems.append(f"flash write outside the store at 0x{addr:08X}")
        elif cr & 0x80000000 or not cr & 1:
            self.problems.append(f"flash write at 0x{addr:08X} without unlock and PG")
        elif self.r32(addr) != 0xFFFFFFFF:
            self.problems.append(f"flash write to a programmed word at 0x{addr:08X}")
        elif addr % 4 or size != 4:
            self.problems.append(f"flash write of size {size} at 0x{addr:08X}")
        self.programs += 1

    def on_write(self, uc, access, addr, size, value, _):
        """record peripheral write."""
        self.flush()
        self.pending.append((addr, value & 0xFFFFFFFF))

    def on_read(self, uc, access, addr, size, value, _):
        """model peripheral read."""
        self.flush()
        if addr == USART3 + 0x24:
            self.pending.append((-1, 0))  # RXNE is cleared by read of RDR

    def flush(self):
        """apply side effects of writes made since last access."""
        pend, self.pending = self.pending, []
        for a, v in pend:
            sh = self.sh
            if a == -1:
                sh[USART3 + 0x1C] &= ~0x20
                self.w32(USART3 + 0x1C, sh[USART3 + 0x1C])
            elif a == RCC:
                v = (v & ~0x02020400) | ((v & 0x10000) << 1) | ((v & 0x100) << 2) | ((v & 0x1000000) << 1)
                self.w32(a, v)
            elif a == RCC + 8:
                self.w32(a, (v & ~0xC) | ((v & 3) << 2))
            elif a == FLASHR + 8:
                if v == 0x45670123:
                    self.flash_key = 1
                elif v == 0xCDEF89AB and self.flash_key == 1:
                    sh[FLASHR + 0x14] &= ~0x80000000
                    self.flash_key = 0
                self.w32(FLASHR + 0x14, sh[FLASHR + 0x14])
            elif a == FLASHR + 0x10:
                sh[a] &= ~v
                self.w32(a, sh[a])
            elif a == FLASHR + 0x14:
                if sh[a] & 0x80000000:
                    self.problems.append("write to FLASH_CR while locked")
                else:
                    if v & 0x10000 and v & 2:  # STRT with PER
                        pnb, bker = (v >> 3) & 0x7F, (v >> 11) & 1
                        base = FLASH + (0x40000 if bker else 0) + pnb * 0x800
                        if not (NV_BASE <= base < NV_END):
                            self.problems.append(f"erase outside the store at 0x{base:08X}")
                        else:
                            self.uc.mem_write(base, b"\xFF" * 0x800)
                            self.erases += 1
                    sh[a] = v & ~0x10000
                self.w32(a, sh[a])
            elif a in (ADC1 + 8, ADC2 + 8):
                base = a - 8
                if v & 1:
                    sh[base] |= 1
                    self.w32(base, sh[base])
                sh[a] = (sh.get(a, 0) & 0x05) | (v & 0x7FFFFFFF)  # ADEN and ADSTART stay set; calibration completes at once
                self.w32(a, sh[a])
            elif a == USART3 + 0x20:
                sh[USART3 + 0x1C] &= ~v
                self.w32(USART3 + 0x1C, sh[USART3 + 0x1C])
            elif a == USART3 + 0x18:
                if v & 8:
                    sh[USART3 + 0x1C] &= ~0x20
                    self.w32(USART3 + 0x1C, sh[USART3 + 0x1C])
            elif a == USART3 + 0x28:
                if not self.r32(GPIOB + 0x14) & (1 << 5):
                    self.problems.append("RS-485 character sent with the driver disabled")
                self.tx.append(v & 0xFF)
                sh[USART3 + 0x1C] |= 0xC0
                self.w32(USART3 + 0x1C, sh[USART3 + 0x1C])
            elif a == LPUART1 + 0x28:
                self.console.append(v & 0xFF)
            elif a == DMA1 + 4:
                sh[DMA1] &= ~v
                self.w32(DMA1, sh[DMA1])
            elif a & ~0xC00 in (GPIOA + 0x18, GPIOA + 0x28) and a < GPIOA + 0x1000:
                port = a & ~0xFF & ~0x300 | (a & 0xC00)
                port = a - (a & 0xFF)
                odr = self.r32(port + 0x14)
                new = ((odr | (v & 0xFFFF)) & ~(v >> 16)) if a & 0xFF == 0x18 else odr & ~v
                self.w32(port + 0x14, new)
                if port == GPIOA and (odr ^ new) & (1 << 5):
                    self.led += 1
                if port == GPIOC and (odr ^ new) & (1 << 8):
                    self.rate_pin += 1

    # --------------------------------------------------------------- execution
    def on_loop(self, uc, addr, size, _):
        """mark point between two passes: main loop is about to call rs485_frame()."""
        if self.skip:
            self.skip = False
        else:
            self.at_loop = True
            uc.emu_stop()

    def run_main(self):
        """run main loop up to its next pass."""
        self.at_loop = False
        self.skip = (self.pc == self.loop)
        try:
            self.uc.emu_start(self.pc | 1, 0, timeout=20_000_000)
        except UcError as e:
            raise RuntimeError(f"{e} at pc 0x{self.uc.reg_read(UC_ARM_REG_PC):08X}; {self.problems}")
        self.pc = self.uc.reg_read(UC_ARM_REG_PC)
        if not self.at_loop:
            raise RuntimeError(f"main loop not reached; pc 0x{self.pc:08X} ({self.where(self.pc)})")
        self.flush()

    def where(self, pc):
        """return name of function that holds address."""
        best = max((a, n) for n, a in self.sym.items() if (a & ~1) <= pc and a >= FLASH)
        return best[1]

    SAVE = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
            UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12,
            UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_XPSR, UC_ARM_REG_FPSCR] + [UC_ARM_REG_D0 + k for k in range(16)]

    def irq(self, handler):
        """run one irq handler to completion, as exception entry and return would."""
        uc = self.uc
        if uc.reg_read(UC_ARM_REG_BASEPRI):
            raise RuntimeError("interrupt delivered while the register lock is held")
        saved = [uc.reg_read(r) for r in self.SAVE]
        uc.reg_write(UC_ARM_REG_SP, (saved[13] - 0x80) & ~7)
        uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        try:
            uc.emu_start(self.sym[handler] | 1, STOP, timeout=5_000_000)
        except UcError as e:
            raise RuntimeError(f"{e} in {handler} at pc 0x{uc.reg_read(UC_ARM_REG_PC):08X}; {self.problems}")
        if uc.reg_read(UC_ARM_REG_PC) != STOP:
            raise RuntimeError(f"{handler} did not return")
        self.flush()
        for r, v in zip(self.SAVE, saved):
            uc.reg_write(r, v)

    # ------------------------------------------------------------ bench
    def sample_tick(self):
        """deliver one output sample period: 16 scans of every pair, by DMA."""
        n = len(self.signals)
        ccr, count, buf = self.r32(DMA1 + 0x08), self.r32(DMA1 + 0x0C), self.r32(DMA1 + 0x14)
        if not ccr & 1 or count != 32 * n:
            raise RuntimeError(f"DMA channel not as expected: CCR 0x{ccr:X}, count {count}")
        scan = self.t_samples * 16 + np.arange(16)
        words = np.zeros((16, n), dtype=np.uint32)
        for p, s in enumerate(self.signals):
            t = scan / F_SCAN + p * T_CONV
            pv, pi = s.codes(t)
            cv = np.clip(np.rint(pv / 3.3 * 4096 + self.rng.normal(0, 0.7, 16)), 0, 4095).astype(np.uint32)
            ci = np.clip(np.rint(pi / 3.3 * 4096 + self.rng.normal(0, 0.7, 16)), 0, 4095).astype(np.uint32)
            master, slave = (cv, ci) if s.v_on_master else (ci, cv)
            words[:, p] = master | (slave << 16)
        self.uc.mem_write(buf + self.half * 64 * n, words.astype("<u4").tobytes())
        self.sh[DMA1] |= 0x4 if self.half == 0 else 0x2  # HTIF1, then TCIF1
        self.w32(DMA1, self.sh[DMA1])
        self.half ^= 1
        self.t_samples += 1
        self.irq("DMA1_Channel1_IRQHandler")

    def step(self):
        """run one pass: irqs of one sample period, then main loop once."""
        self.sample_tick()
        while self.t_samples / (F_SCAN / 16) * 1000 >= self.ms + 1:
            self.ms += 1
            self.irq("SysTick_Handler")
        self.pump_uarts()
        self.run_main()

    def pump_uarts(self):
        """move chars of console and of RS-485 port."""
        for _ in range(4096):  # console: ring is drained
            if not self.r32(LPUART1) & 0x80:
                break
            self.irq("LPUART1_IRQHandler")
        for _ in range(600):  # RS-485: bytes are sent until driver is released
            cr1 = self.r32(USART3)
            if not cr1 & 0xC0:
                break
            self.irq("USART3_IRQHandler")

    def run(self, seconds):
        """run node for given time (s)."""
        for _ in range(int(seconds * F_SCAN / 16)):
            self.step()

    def glitch(self):
        """flag framing error w/ no char, as spike on idle bus may cause."""
        self.sh[USART3 + 0x1C] |= 0x2
        self.w32(USART3 + 0x1C, self.sh[USART3 + 0x1C])
        self.irq("USART3_IRQHandler")

    def request(self, pdu, expect_reply=True, garble=False, framing_error_at=None):
        """send one Modbus frame as bus would deliver it; return reply."""
        frame = bytes(pdu) + struct.pack("<H", crc16(pdu))
        if garble:
            frame = frame[:-1] + bytes([frame[-1] ^ 0x55])
        self.tx = bytearray()
        for k, b in enumerate(frame):
            self.w32(USART3 + 0x24, b)
            self.sh[USART3 + 0x1C] |= 0x22 if k == framing_error_at else 0x20
            self.w32(USART3 + 0x1C, self.sh[USART3 + 0x1C])
            self.irq("USART3_IRQHandler")
        self.sh[USART3 + 0x1C] |= 0x800  # receiver timeout
        self.w32(USART3 + 0x1C, self.sh[USART3 + 0x1C])
        self.irq("USART3_IRQHandler")
        for _ in range(4):
            self.step()
        if self.r32(GPIOB + 0x14) & (1 << 5):
            self.problems.append("driver enable still high after the reply")
        reply = bytes(self.tx)
        if expect_reply:
            if len(reply) < 5 or crc16(reply) != 0:
                raise RuntimeError(f"bad reply to {frame.hex(' ')}: {reply.hex(' ')}")
        elif reply:
            raise RuntimeError(f"unexpected reply: {reply.hex(' ')}")
        return reply

    def read_ir(self, addr, count, slave=None):
        """read input regs; return words."""
        r = self.request(struct.pack(">BBHH", slave or self.address, 4, addr, count))
        assert r[1] == 4 and r[2] == 2 * count, r.hex(" ")
        return struct.unpack(f">{count}H", r[3:3 + 2 * count])

    def write_hr(self, addr, words):
        """write holding regs."""
        body = b"".join(struct.pack(">H", w) for w in words)
        r = self.request(struct.pack(">BBHHB", self.address, 16, addr, len(words), len(body)) + body)
        assert r[1] == 16, r.hex(" ")

    @property
    def address(self): return {"SRC": 1, "SY1": 2, "SY2": 3, "SY3": 4, "SY4": 5}[self.node]  # slave address of node

    def point(self, p):
        """return eight values of point."""
        # 16 regs per point from IR 4: eight float32 values, high word first
        w = self.read_ir(4 + 16 * p, 16)
        v, i, f, ang, pf, pw, q, s = struct.unpack(">8f", struct.pack(">16H", *w))
        return dict(v=v, i=i, f=f, ang=ang, pf=pf, p=pw, q=q, s=s)

    def energy(self, p):
        """return energy total of point (mWh)."""
        hi, lo = self.read_ir(82 + 2 * p, 2)
        return (hi << 16) | lo

    def diag(self):
        """return blocks, overruns, rx_errors of board_diag_t."""
        return struct.unpack("<III", self.uc.mem_read(self.sym["g_diag"], 12))

    def flash_image(self): return bytes(self.uc.mem_read(FLASH, FLASH_SIZE))  # return flash content

# ------------------------------------------------------------------- checks
class Checks:
    """check counter."""
    def __init__(self): self.n = self.bad = 0  # checks run, checks failed
    def ok(self, cond, what):
        """count one check; print result."""
        self.n += 1
        if not cond:
            self.bad += 1
        print(("  pass  " if cond else "  FAIL  ") + what)
    def near(self, got, want, tol, what):
        """check that got is within tol of want."""
        self.ok(abs(got - want) <= tol, f"{what}: {got:.4f} (expected {want:.4f} +/- {tol:g})")

K12, K24 = 10 / 160, 10 / 310

def src_signals():
    """return signals of source node."""
    return [Signal(True, K12, 0.01, 12.0, 2.0, 30.0, mid=1.66),            # MP-01 mains
            Signal(False, K12, 0.01, 12.0, 1.0, dc=True),                  # MP-02 DC point (pair 2: V on ADC2)
            Signal(True, K12, 0.01, 12.1, 0.5, -20.0, f=59.95, mid=1.64),  # MP-03
            Signal(True, K24, 0.02, 24.0, 1.5, 49.0)]                      # MP-04

def sy_signals():
    """return signals of switchyard node."""
    return [Signal(True, K24, 0.05, 24.0, 0.444, 0.0, mid=1.655),  # switchyard 24 V side
            Signal(False, K12, 0.05, 12.0, 0.5, 25.0)]             # load feed (pair 2: V on ADC2)

def check_registers(c, n, pairs):
    """check published values against signals."""
    print("peripheral registers after start-up")
    pll = n.r32(RCC + 0x0C)
    c.ok(pll & 3 == 3 and (pll >> 4) & 15 == 5 and (pll >> 8) & 127 == 85 and (pll >> 25) & 3 == 0 and pll & (1 << 24),
         f"PLL: crystal source, M 6, N 85, R 2 (PLLCFGR 0x{pll:08X})")
    c.ok(n.r32(RCC + 8) & 0xC == 0xC and n.r32(RCC + 8) & 0x3FF0 == 0, "system clock on the PLL; AHB and APB prescalers 1")
    c.ok(n.r32(FLASHR) & 15 == 4, "flash latency 4 wait states")
    c.ok(not n.r32(PWR + 0x80) & 0x100, "voltage range 1 boost mode")
    c.ok(n.r32(TIM6 + 0x2C) == 2766 and n.r32(TIM6 + 0x28) == 0 and (n.r32(TIM6 + 4) >> 4) & 7 == 2 and n.r32(TIM6) & 1,
         "TIM6: reload 2766, prescaler 0, update as trigger output, running")
    ccr = n.r32(ADC12C + 8)
    c.ok(ccr & 31 == 6 and (ccr >> 14) & 3 == 2 and ccr & (1 << 13) and (ccr >> 16) & 3 == 3,
         f"ADC12 common: dual regular simultaneous, DMA 12-bit circular, clock HCLK/4 (CCR 0x{ccr:08X})")
    cf1, cf2 = n.r32(ADC1 + 0x0C), n.r32(ADC2 + 0x0C)
    c.ok((cf1 >> 5) & 31 == 13 and (cf1 >> 10) & 3 == 1 and (cf1 >> 3) & 3 == 0 and not cf1 & (1 << 13),
         f"ADC1: trigger TIM6_TRGO rising, 12 bit, single conversion per trigger (CFGR 0x{cf1:08X})")
    c.ok((cf2 >> 10) & 3 == 0, "ADC2: no trigger of its own (follows ADC1)")
    want1 = [1, 15, 6, 8][:pairs]
    want2 = [2, 17, 7, 9][:pairs]
    for name, base, want in (("ADC1", ADC1, want1), ("ADC2", ADC2, want2)):
        sq = n.r32(base + 0x30)
        got = [(sq >> (6 * (k + 1))) & 31 for k in range(pairs)]
        c.ok(sq & 15 == pairs - 1 and got == want, f"{name} sequence {got} (expected {want})")
        smp = n.r32(base + 0x14) | (n.r32(base + 0x18) << 30)
        c.ok(all((smp >> (3 * ch)) & 7 == 4 for ch in want), f"{name} sampling time 47.5 cycles on its channels")
        cr = n.r32(base + 8)
        c.ok(cr & 1 and cr & (1 << 28) and not cr & (1 << 29), f"{name} enabled, regulator on")
    c.ok(n.r32(ADC1 + 8) & 4, "ADC1 conversions started (armed for the trigger)")
    dcc = n.r32(DMA1 + 8)
    c.ok(dcc & 0x0FAF == 0x0AA7 and n.r32(DMA1 + 0x10) == ADC12C + 0x0C and n.r32(DMAMUX1) & 0x7F == 5,
         f"DMA1 channel 1: circular, 32-bit, half and full interrupts, from the ADC12 common data register, request ADC1 (CCR 0x{dcc:04X})")
    c.ok(n.r32(USART3 + 0x0C) == round(F_SYS / 115200), f"USART3 divider {n.r32(USART3 + 0x0C)} (115 200 bit/s)")
    c.ok(n.r32(USART3 + 0x14) == 202 and n.r32(USART3 + 4) & (1 << 23), "USART3 receiver timeout 202 bit times, enabled")
    cr1 = n.r32(USART3)
    c.ok(cr1 & 0x1D == 0x0D and cr1 & (1 << 5) and cr1 & (1 << 26) and not cr1 & ((1 << 12) | (1 << 28) | (1 << 10)),
         f"USART3 8N1, receive and timeout interrupts on (CR1 0x{cr1:08X})")
    mb, afh = n.r32(GPIOB), n.r32(GPIOB + 0x24)
    c.ok((mb >> 20) & 15 == 0b1010 and (afh >> 8) & 0xFF == 0x77 and (mb >> 10) & 3 == 1 and mb & 3 == 3,
         "PB10 and PB11 on USART3 (AF7), PB5 output, PB0 analog")
    ma = n.r32(GPIOA)
    c.ok(ma & 15 == 15 and (ma >> 8) & 3 == 3 and (ma >> 4) & 3 == 2 and (n.r32(GPIOA + 0x20) >> 8) & 15 == 12 and (ma >> 10) & 3 == 1,
         "PA0, PA1, PA4 analog; PA2 on LPUART1 (AF12); PA5 output")
    c.ok(n.r32(LPUART1 + 0x0C) == round(256 * F_SYS / 115200), "LPUART1 divider for 115 200 bit/s")
    c.ok(n.r32(0xE000E014) == 169999 and n.r32(0xE000E010) & 7 == 7, "SysTick: 1 ms from the core clock, interrupt on")
    ipr = lambda irq: n.uc.mem_read(0xE000E400 + irq, 1)[0] >> 4
    c.ok(ipr(39) == 0 and ipr(11) == 1 and n.uc.mem_read(0xE000ED23, 1)[0] >> 4 == 2 and ipr(91) == 3,
         "priorities: USART3 0, DMA1 channel 1 at 1, SysTick 2, LPUART1 3")
    en = lambda irq: n.r32(0xE000E100 + 4 * (irq // 32)) >> (irq % 32) & 1
    c.ok(en(39) and en(11) and en(91), "the three peripheral interrupts are enabled")

def main():
    """run emulated test."""
    c = Checks()

    print("NODE-SRC image")
    n = Node("SRC", src_signals())
    check_registers(c, n, 4)
    n.run(2.5)
    text = n.console.decode(errors="replace")
    c.ok("NODE-SRC  address 1" in text and "24 MHz crystal" in text and "3839.90 Hz" in text, "banner: node, crystal clock, sample rate 3839.90 Hz")
    c.ok("no calibration stored" in text and "energy starts at zero" in text, "banner: blank store reported")
    st = n.read_ir(0, 4) + n.read_ir(80, 2)
    c.ok(st[0] & 1 and not st[0] & 2 and not st[0] & 0xC, f"status 0x{st[0]:04X}: sampling, calibration not valid, no over-range, no fault")
    c.ok(st[1] == 2 and abs(((st[2] << 16) | st[3]) - 2500) < 150 and st[4] == 2 and st[5] == 0, f"heartbeat {st[1]}, node time {(st[2] << 16) | st[3]} ms, firmware 0x{st[4]:04X}")
    print("measurement through the whole chain (synthetic converter codes to Modbus registers)")
    sig = src_signals()
    for p, s in enumerate(sig):
        m = n.point(p)
        if s.dc:
            c.near(m["v"], s.vrms, 0.03, f"point {p} DC voltage, V")
            c.near(m["i"], s.irms, 0.02, f"point {p} DC current, A")
            c.near(m["p"], s.vrms * s.irms, 0.3, f"point {p} DC power, W")
        else:
            c.near(m["v"], s.vrms, s.vrms * 0.002, f"point {p} voltage, V")
            c.near(m["i"], s.irms, s.irms * 0.004, f"point {p} current, A")
            c.near(m["f"], s.f, 0.01, f"point {p} frequency, Hz")
            c.near(m["ang"], s.lag_deg, 0.3, f"point {p} angle, degrees")
            c.near(m["p"], s.vrms * s.irms * math.cos(math.radians(s.lag_deg)), s.vrms * s.irms * 0.006, f"point {p} active power, W")
    c.near(n.var("s_mid", "<f"), 1.66 / 3.3 * 16384, 15, "tracked mid-rail of point 0 voltage, counts (pin at 1.66 V; the estimate ripples by 13 counts)")
    c.ok(n.rate_pin == n.t_samples and n.var("g_diag") == n.t_samples, f"test pin toggled once per output sample ({n.rate_pin})")
    c.ok(4 <= n.led <= 6, f"heartbeat lamp toggled {n.led} times in 2.5 s")

    print("bus behaviour")
    n.request(struct.pack(">BBHH", 3, 4, 0, 5), expect_reply=False)
    c.ok(True, "frame for another address: no reply")
    n.request(struct.pack(">BBHH", 1, 4, 0, 5), expect_reply=False, garble=True)
    c.ok(True, "frame with a bad CRC: no reply")
    r = n.request(struct.pack(">BBHH", 1, 4, 300, 2))
    c.ok(r[1] == 0x84 and r[2] == 2, "unmapped register: exception 2")
    r = n.read_ir(0, 5)
    c.ok(r[0] & 1, "node answers again afterwards")
    n.request(struct.pack(">BBHH", 1, 4, 0, 5), expect_reply=False, framing_error_at=3)
    c.ok(n.diag()[2] == 1, "frame with a framing error on one character: no reply, one line error counted")
    n.glitch()
    r = n.read_ir(0, 5)
    c.ok(r[0] & 1 and n.diag()[2] == 1, "line error with no character on an idle bus: the next frame is still answered")
    n.request(struct.pack(">BBHHB", 0, 16, 100, 2, 4) + struct.pack(">I", 5_000_000), expect_reply=False)
    n.run(0.25)
    t = n.read_ir(2, 2)
    c.ok(abs(((t[0] << 16) | t[1]) - 5_000_250) < 150, f"broadcast node-time set applied, no reply ({(t[0] << 16) | t[1]} ms)")

    print("calibration write, store, and restart")
    e_before = n.energy(0)
    g = struct.unpack(">HH", struct.pack(">f", 1.01))
    o = struct.unpack(">HH", struct.pack(">f", 0.0))
    n.write_hr(110, list(g) + list(o))
    progs = n.programs
    n.run(1.0)
    c.ok(n.programs == progs, "no flash write within 1 s of the calibration write")
    n.run(1.5)
    c.ok(n.programs - progs == 24, f"one 96-byte record programmed 2 s after the write ({n.programs - progs} words)")
    c.ok(n.console.decode(errors="replace").count("store: record written") == 1, "console reports the record")
    m = n.point(0)
    c.near(m["v"], 12.0 * 1.01, 0.03, "point 0 voltage with gain 1.01, V")
    c.ok(n.read_ir(0, 1)[0] & 2, "status: calibration valid after the host write")
    e_stored = n.energy(0)
    c.ok(e_stored > e_before > 0, f"energy of point 0 counting ({e_before} then {e_stored} mWh)")
    c.ok(n.diag()[0] > 0 and n.diag()[1] == 0, "no sampling overruns reported")

    n2 = Node("SRC", src_signals(), flash=n.flash_image(), seed=2)
    n2.run(1.2)
    text2 = n2.console.decode(errors="replace")
    c.ok("calibration restored, energy restored" in text2, "restart: banner reports calibration and energy restored")
    c.near(n2.point(0)["v"], 12.0 * 1.01, 0.03, "restart: point 0 voltage still scaled by 1.01, V")
    e2 = n2.energy(0)
    c.ok(e_stored - 12 <= e2 <= e_stored + 12, f"restart: energy resumes from the stored count ({e2} mWh against {e_stored})")
    c.ok(n2.read_ir(0, 1)[0] & 2, "restart: calibration valid")
    for node in (n, n2):
        c.ok(not node.problems, "no register-model violations" + (": " + "; ".join(node.problems[:4]) if node.problems else ""))

    print("store wear-levelling (image built with a 50 ms store period)")
    f = Node("SRC", src_signals(), extra="-DNV_ENERGY_PERIOD_MS=50u")
    f.run(9.0)
    img = f.flash_image()
    hdr = [struct.unpack("<II", img[a - FLASH:a - FLASH + 8]) for a in (NV_BASE, NV_BASE + 0x2000)]
    c.ok(f.erases >= 4, f"sector left behind erased in the background ({f.erases} pages, {f.programs // 24} records written)")
    live = [h for h in hdr if h[0] == 0x564E4753]
    c.ok(len(live) >= 1 and max(h[1] for h in live) >= 2, f"active sector header sequence {max(h[1] for h in live)} (2 or more after a sector change)")
    e_run = f.energy(0)
    f2 = Node("SRC", src_signals(), extra="-DNV_ENERGY_PERIOD_MS=50u", flash=img)
    f2.run(0.3)
    e_back = f2.energy(0)
    c.ok(e_run - 3 <= e_back <= e_run + 3, f"restart after a sector change: energy {e_back} mWh against {e_run}")
    c.ok(f.diag()[1] == 0, "no overruns across the sector change")
    c.ok(not f.problems and not f2.problems, "no register-model violations" + (": " + "; ".join((f.problems + f2.problems)[:4]) if f.problems or f2.problems else ""))

    print("NODE-SY1 image")
    s = Node("SY1", sy_signals())
    check_registers(c, s, 2)
    s.run(1.5)
    c.ok("NODE-SY1  address 2" in s.console.decode(errors="replace"), "banner: NODE-SY1, address 2")
    for p, g in enumerate(sy_signals()):
        m = s.point(p)
        c.near(m["v"], g.vrms, g.vrms * 0.002, f"point {p} voltage, V")
        c.near(m["i"], g.irms, g.irms * 0.006, f"point {p} current, A")
        c.near(m["p"], g.vrms * g.irms * math.cos(math.radians(g.lag_deg)), g.vrms * g.irms * 0.008, f"point {p} active power, W")
    r = s.request(struct.pack(">BBHH", 2, 4, 4 + 32, 16))
    c.ok(r[1] == 0x84, "point 2 does not exist on a switchyard node: exception")
    c.ok(not s.problems, "no register-model violations" + (": " + "; ".join(s.problems[:4]) if s.problems else ""))

    print(f"\n{c.n - c.bad} of {c.n} checks passed")
    print("--- console of the first run, last lines ---")
    print("\n".join(text.replace("\r", "").splitlines()[:5] + ["..."] + n.console.decode(errors="replace").replace("\r", "").splitlines()[-6:]))
    return 1 if c.bad else 0

if __name__ == "__main__":
    sys.exit(main())
