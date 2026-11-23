# Measurement node firmware for the NUCLEO-G474RE

SGTB Module 4, Citadel Grid Solutions, ELEC 421-81. Author: Thomas Bassett.
Issued November 20, 2026, with package `m4fw_step5.zip`.

This folder is the board-specific layer of the measurement node: clock, sampling
engine, RS-485 driver, flash store, console, and the main program. The
measurement math, the register bank, and the Modbus slave are the portable
libraries of `firmware/lib/`, unchanged; the simulator and this firmware run
the same code above the hardware.

**State.** The five images compile without warnings and pass an emulated run
(`tests/emu_nucleo.py`, 109 checks). Nothing in this folder has run on a board.
`BRINGUP.md` gives the order in which the hardware-dependent behaviour is to
be confirmed.

## Build and program

    make NODE=SRC            # NODE-SRC, address 1, four points
    make NODE=SY1            # NODE-SY1, address 2, two points (SY2, SY3, SY4: addresses 3, 4, 5)
    make every               # all five images
    make flash NODE=SY1      # st-flash by default; FLASH_TOOL=cube, openocd, or drag

Requirements: `arm-none-eabi-gcc` with newlib (Debian and Ubuntu packages
`gcc-arm-none-eabi` and `libnewlib-arm-none-eabi`). The ST device header, the
low-layer driver headers, the start-up file, and the CMSIS core headers are in
`vendor/` with their licenses; STM32CubeIDE is not required. `prebuilt/` holds
the five images built from this source with GCC 13.2.1, for a board that is to
be programmed before the toolchain is installed (copy the `.bin` to the
`NODE_G474RE` drive).

The node identity is fixed at build time. A board is a given node because of
the image it holds.

## Files

| File | Content |
| --- | --- |
| `board.h` | Node identity, clock and sampling constants, nominal front-end scaling, interrupt priorities, and the interfaces of the modules below. |
| `board_lock.h` | Register lock of the node (`NODE_REGS_LOCK`), forced into every source file so that the portable register bank is built with it. |
| `main.c` | Start-up order, register lock, millisecond tick, main loop, console banner and report, and the fault handler. |
| `clock.c` | 170 MHz from the 24 MHz crystal; fallback to the internal oscillator. |
| `sampling.c` | Pair table of the node, TIM6, ADC1 and ADC2 in dual mode, DMA, oversampling, mid-rail tracking, and scaling. |
| `rs485.c` | USART3 half-duplex driver with Modbus RTU framing. |
| `nv.c` | Flash store of the calibration constants and energy counters. |
| `dbg.c` | Console on the ST-LINK virtual COM port. |
| `stubs.c` | Four unused system calls of the C library. |
| `stm32g474re.ld` | Memory layout: program in flash bank 1, store at the end of bank 2. |
| `Makefile` | Build, node selection, and programming. |
| `vendor/` | ST and Arm files, unmodified. |
| `prebuilt/` | The five images and their SHA-256 sums. |

## Design as implemented

Pins, peripherals, and rates follow the Measurement Front End design, revision
1.1, and Design Report Table F.M4.15.

| Item | Implementation |
| --- | --- |
| System clock | 24 MHz crystal X3, PLL M 6, N 85, R 2: 170 MHz, range 1 boost, 4 wait states. If the crystal does not start, the internal 16 MHz oscillator feeds the PLL, the banner says so, and LD2 blinks at 5 Hz instead of 1 Hz. |
| Conversion trigger | TIM6 update, reload 2767: 61 438.4 Hz. The design value of 61.44 kHz is not an integer division of 170 MHz; the difference is 26 ppm. |
| Output sample rate | 16 conversions per sample: 3839.90 Hz. The meter is configured with this value, so frequency and energy are not biased by the 26 ppm. |
| Converters | ADC1 master and ADC2 slave, dual regular simultaneous mode, 12 bit, clock HCLK / 4 = 42.5 MHz, sampling time 47.5 cycles, 1.41 us per conversion. Each trigger scans every pair of the node; the voltage and current of a pair are sampled at the same instant, and successive pairs 1.41 us apart. |
| Pair 1 | Voltage PA0 (ADC1_IN1), current PA1 (ADC2_IN2). |
| Pair 2 | Voltage PA4 (ADC2_IN17), current PB0 (ADC1_IN15). |
| Pairs 3 and 4 (source node) | Voltage PC0 (ADC1_IN6), current PC1 (ADC2_IN7); voltage PC2 (ADC1_IN8), current PC3 (ADC2_IN9). |
| Data path | DMA1 channel 1, circular, from the ADC12 common data register; an interrupt at each half of the buffer, 3840 per second. |
| Output sample | Sum of 16 conversions divided by 4: a 14-bit value, 0.2014 mV per count at the pin. |
| Mid-rail | AC channels: tracked as the long-run mean of the channel (a high-pass filter with its corner at 0.15 Hz). DC point: fixed at mid-scale, offset left to calibration. |
| Nominal scaling | Dividers 10/160 (12 V tier) and 10/310 (24 V tier); amplifier gain 20; shunts 0.05, 0.02, and 0.01 ohm. The calibration constants of the register map (gain and offset per channel) are applied after it. |
| Fieldbus | USART3, PB10 transmit, PB11 receive, PB5 driver enable (high to transmit); 115 200 bit/s, 8N1; frame end at 1.75 ms of silence (202 bit times, receiver timeout of the USART). |
| Node time | SysTick, 1 kHz. |
| Console | LPUART1 on PA2 (ST-LINK virtual COM port), 115 200 bit/s, output only. |
| Store | Last 16 KB of flash (0x0807C000 to 0x0807FFFF), two 8 KB sectors, 96-byte records with CRC-32. A record is written 2 s after the last calibration write, every 60 s while an energy counter has moved, and at once on an energy reset. |
| Indicators | LD2 (PA5) heartbeat. PC8 toggles once per output sample: a 1919.95 Hz square wave when the sampling rate is right. |

### Interrupts and the register lock

| Priority | Source | Work |
| --- | --- | --- |
| 0 | USART3 | Characters of a frame in and out. Never held off by the lock. |
| 1 | DMA1 channel 1 | One output sample of every point; the window arithmetic when a window closes. |
| 2 | SysTick | Node time and heartbeat. |
| 3 | LPUART1 | Console output. |
| none | Main loop | Serves a frame, acts on the host's writes, runs the store, and reports once per second. |

The lock raises BASEPRI to priority 1. The main loop holds it for the
live-to-latched copy of a read, for the host-write service, and for reads of
the energy counters.

### Console

After the banner, one line per point and one status line are printed each second:

    p0 V 12.114 I 1.9990 f 60.000 P 20.970 pf 0.866 mid 8228.9 8235.6
    t 5001489 samples 15360 overruns 0 mb ok 10 other 1 crc 1 exc 1 line 1 status 0x0003

`mid` is the tracked mid-rail of the voltage and current channel in 14-bit
counts (8192 is 1.65 V). `samples` rises by 3840 per second. `overruns` counts
sampling interrupts served late and is to stay at zero. `mb ok` counts frames
answered; `other`, frames for another address; `crc`, `exc`, and `line`, CRC
errors, exception replies, and frames dropped for a line error. `status` is
the health register.

## Decisions taken in this layer

These were not fixed by the design documents and are open to change.

| Decision | Basis |
| --- | --- |
| Pair p of the front end is point p of the register map. On NODE-SRC: MP-01, MP-02, MP-03, MP-04 on pairs 1 to 4. On a switchyard node: the 24 V side on pair 1 and the load feed on pair 2. | Order of the register map; the wiring list is to confirm it. |
| The DC point MP-02 uses the 12 V divider and the 0.01 ohm shunt, centred on mid-rail. | Placeholder until Module 1 fixes the divider and shunt; one line of the pair table in `sampling.c`. |
| Node identity at build time rather than by straps. | No strap pins are assigned in Table F.M4.15. |
| PC8 as a sample-rate test pin and PA5 (LD2) as heartbeat. | Free pins; either is removed by deleting one line. |
| Calibration reported valid (status bit 1) only after constants are restored from flash or written by the host. | An uncalibrated node is then visible to the host. |
| The store is disabled when the flash is not in the dual-bank layout. | In dual-bank layout a write in bank 2 does not stall the program in bank 1; in single-bank layout an erase would stop the sampling interrupt for about 22 ms. Dual bank is the factory state. |
| No correction for the 16-conversion average. | It lowers a 60 Hz reading by 0.04%; the gain calibration at 60 Hz absorbs it. |

## Not in this package

Trip input (PB4), the ICD-4 link on USART1, the source-position input, the
boot self-test, and the watchdog. PB4, PC4, and PC5 are left in their reset
state. The fault record (`firmware/lib/capture`) is built in and armed from
start-up, and the frames of the link are in `firmware/lib/icd4`; neither is
connected to a pin yet. The record is triggered by a call of `node_app_trip`
from the edge interrupt of PB4 once the polarity of TRIP is confirmed with
Module 2. The record adds about 18.5 KB of RAM.

## Emulated test

`python3 tests/emu_nucleo.py` (or `make emu` at the package root) runs the
real images on the Unicorn Cortex-M4 emulator with a register-level model of
the peripherals. It checks the registers programmed at start-up against values
worked out independently, feeds synthetic converter codes through the sampling
interrupt, exchanges Modbus frames through the USART3 interrupt, and restarts
the image on the flash contents it left. In that run the sampling interrupt
took a median of about 1100 instructions on NODE-SRC and at most about 16 200
(a window closing on all four points in one interrupt), against a period of
44 270 processor cycles.

The emulator models register behaviour only: no analog behaviour, no timing
inside a peripheral, and no pre-emption of one interrupt by another. A pass is
evidence about the logic and the configuration values, not about the board.
