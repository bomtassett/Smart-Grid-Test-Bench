# SGTB Module 4 software: package directory

Citadel Grid Solutions, ELEC 421-81. Author: Thomas Bassett. Issued November 22, 2026. Directory revision 11 (comments as directives, with articles dropped).

This directory lists the delivered packages, every file in the current package,
the test suites, and the map from the Module 4 test cases to the code that
serves them.

## Status on November 22, 2026

| Item | State |
| --- | --- |
| Current package | `m4fw_step12.zip`. |
| Test suites | Eight suites, 420 checks, plus 131 register readbacks; all pass on a PC with `make test`. A ninth, the node images on a Cortex-M4 emulator (109 checks), passes with `make emu`. |
| Built and tested | Register map and generator; Modbus RTU slave; node and load-control register banks; measurement math; fault record (trip capture); ICD-4 frames and link supervision; node application layer; simulated nodes and load controller; host poller; historian; supervisory API; demand controller. Board-specific node firmware (clock, sampling engine, RS-485 driver, flash store, console): built for all five nodes and tested on the emulator only. |
| Not yet written | The read of the fault record over the fieldbus (its register layout waits on decision D-28); event and capture tables and endpoints; on the node, the trip input (PB4), the controller link (USART1), the source-position input, the boot self-test, and the watchdog; the controller's chunked record transfer (open item OI-28); stand-in load controller sketch; stand-alone conformance harness and scripted ICD-3 client. |
| Verified on hardware | Nothing. No board was in hand when this package was built. Every timing figure so far is against the simulator or the emulator. The order of confirmation on the board is in `firmware/board/nucleo_g474re/BRINGUP.md`. |
| Testing plan | Proof of Concept Mini-Proposal and Testing Plan, Rev. 07. Case 7 is written for seven automatic steps; decision D-39 is not yet closed in the open points list. |
| Open points list (OPL) | Sheet "SGTB Module 4 - Open Items and Decisions", Rev. 04: decisions D-01 to D-42 (40 open, 2 decided) and actions A-01 to A-09. https://claude.ai/artifact/CaBggHTMFvHLbBcVCvDczm |

## 1. Packages

Each package is cumulative: it contains everything in the one before it.
Only the latest is needed.

| Package | Date | Adds | Status |
| --- | --- | --- | --- |
| `m4fw_step1.zip` | 2026-11-14 | Register-map source and generator; Modbus RTU slave library; node and load-control register banks; unit tests and cross-check. | Superseded by step 2. |
| `m4fw_step2.zip` | 2026-11-14 | Measurement math; node application layer; simulated node on virtual serial ports; host Modbus master and live table; measurement and end-to-end tests. | Superseded by step 3. |
| `m4fw_step3.zip` | 2026-11-14 | Host poller and SQLite historian with alarms, derived quantities, load-control commands, CSV export, and snapshot; host end-to-end test; this directory. | Superseded by step 4. |
| `m4fw_step4.zip` | 2026-11-14 | Supervisory API (ICD-3) with users, roles, and sessions; demand controller for automatic shedding and restoration; load fault rules; API end-to-end test. Reissued the same day with directory revision 03. | Superseded by step 5. |
| `m4fw_step5.zip` | 2026-11-20 | Node firmware for the NUCLEO-G474RE with the ST headers it needs and five prebuilt images; emulated test of the node images; `make fw` and `make emu`; directory revision 04. | Superseded by step 6. |
| `m4fw_step6.zip` | 2026-11-21 | Fault record of the node (per-cycle rows and raw clip about the TRIP edge) and the per-cycle result of the meter it is built from; ICD-4 frame encoding, decoding, and link supervision; two unit-test suites; the five node images rebuilt; directory revision 05. | Superseded by step 7. |
| `m4fw_step7.zip` | 2026-11-22 | Register map 0.2: every measured quantity is a float32 in engineering units, the 10 Hz read is the four header registers and the point blocks with no unused register, and the firmware version joins the energy counters in a 1 Hz block; node register bank, host readers, and tests rebuilt on it; firmware version 0.2; the five node images rebuilt; directory revision 06. | Superseded by step 8. |
| `m4fw_step8.zip` | 2026-11-22 | Fault record word stream at layout version 2: each row value is a float32 (12 words per row), and a row value that is not finite is stored as 0 and flagged; the comments of every hand-written source file rewritten to the source comment convention of `README.md`, with no change to the code (the five node images are byte-identical before and after the comment revision); the five node images rebuilt; directory revision 07. | Superseded by step 9. |
| `m4fw_step9.zip` | 2026-11-22 | The firmware version of each node (IR 80) is compared by the host with the register map version at each 1 Hz read, and a "firmware mismatch" alarm is raised while the two differ; `regmap.yaml` and `gen_regmap.py` taken as pushed to the team repository, with `generated/` regenerated for two shortened descriptions; the comments of every other hand-written source file revised to the convention of `README.md` (plain-language `Purpose`, the item in focus as the subject of each statement, and registers written as `IR 80` and `HR 150`), with no change to the firmware code (the five node images are byte-identical to those of step 8); directory revision 08. | Superseded by step 10. |
| `m4fw_step10.zip` | 2026-11-22 | Source commenting standard of `README.md` applied to every hand-written source file: terms defined in the header, one sentence on every function, one line on every type, and a short name for every variable, field, and constant; no change to the code (the five node images are byte-identical to those of step 8 and step 9); directory revision 09. | Superseded by step 11. |
| `m4fw_step11.zip` | 2026-11-22 | Final file header applied to every hand-written source file: department banner, engineer, dates, project, module (M4 - Supervisory Control and Data Acquisition), level in the ISA112 SCADA model architecture diagram (draft of 2022-01-26), file title, purpose, and notes; no change to the code (the five node images are byte-identical to those of steps 8 to 10); directory revision 10. | Superseded by step 12. |
| `m4fw_step12.zip` | 2026-11-22 | Function descriptions rewritten as directives and articles dropped from all comments; `host/sgtb_modbus.py` and the header of `host/api.py` taken as revised by the engineer; `Modified` set to the date of the last edit; no change to the code (the five node images are byte-identical to those of steps 8 to 11); directory revision 11. | Current. |

All twelve unpack to one folder named `m4fw/`. The folder is intended to become
the Module 4 directory of the team repository.

## 2. Folder layout

| Folder | Role | Runs on |
| --- | --- | --- |
| `regmap/` | The single hand-edited register-map source. | n/a |
| `tools/` | The generator that turns the source into every other map artifact. | PC |
| `generated/` | Generator output. Never edited by hand. | Node, load controller, host |
| `firmware/lib/` | Hardware-independent firmware libraries, C99. | Node and load controller; also compiled on the PC for tests |
| `firmware/board/nucleo_g474re/` | Board-specific node firmware and the vendor files it needs. | Node (NUCLEO-G474RE) |
| `sim/` | Simulated nodes and load controller. | PC (Linux or macOS) |
| `host/` | Host service: poller, historian, and tools. Python 3. | Host virtual machine |
| `tests/` | Test suites and their helpers. | PC |

## 3. Files

Lines are counted on November 22, 2026.

### Root

| File | Lines | Content |
| --- | --- | --- |
| `README.md` | 140 | Summary, layout, and commands. |
| `DIRECTORY.md` | n/a | This document. |
| `Makefile` | 85 | `make test` (all PC suites), `make sim` (simulator), `make host` (poller), `make fw` (node images), `make emu` (node images on the emulator), and `make clean`. |
| `.gitignore` | 5 | Build output, Python caches, and database files. |
| `requirements.txt` | 9 | Python packages: PyYAML, FastAPI, and uvicorn for the host; httpx and websockets for the tests; unicorn and numpy for the emulated test. |

### regmap/ and tools/

| File | Lines | Content |
| --- | --- | --- |
| `regmap/regmap.yaml` | 133 | Register map of the nodes (ICD-2) and the load control unit (ICD-6): addresses, types, units, status bits, node and point lists, and segment settings. The only file edited to change the map (CST-M4-14). |
| `tools/gen_regmap.py` | 343 | Validates the source (no overlap, unique tags and addresses, segment 1 utilization at or under 50%) and writes the four files of `generated/`. `--check` fails when `generated/` is out of date. |

### generated/

| File | Lines | Content |
| --- | --- | --- |
| `regmap_node.h` | 95 | C header for the node firmware: addresses, offsets of the float32 fields, status and control bits, point indices, and the sizes of the 10 Hz and 1 Hz reads. |
| `regmap_lcu.h` | 21 | C header for the load control unit slave. |
| `tags.json` | 1837 | Tag table (127 tags) and poll plan for the host. |
| `register_map.md` | 193 | Register tables, node and point list, bus budget, and tag list, for ICD-2 and ICD-6. |

### firmware/lib/

| File | Lines | Content |
| --- | --- | --- |
| `modbus_rtu/modbus_slave.h` | 88 | Interface of the frame-level Modbus RTU slave. |
| `modbus_rtu/modbus_slave.c` | 240 | Function codes 03, 04, 06, and 16; exceptions 01 to 03; broadcast writes; CRC. |
| `regs/node_regs.h` | 97 | Interface of the node register bank. |
| `regs/node_regs.c` | 230 | Measured values held as float32 with no scaling or range limit, the fault flag for a value that is not finite, latched reads, calibration words, time set, energy reset, and control word. |
| `regs/lcu_regs.h` | 88 | Register bank of the load control unit, header-only: shed command, contact states, fault flags, and heartbeat. The file handed to Module 3 with `modbus_slave.*` and `regmap_lcu.h`. |
| `meter/meter.h` | 113 | Interface and configuration of the per-point measurement math, and the result of the cycle that closed last. |
| `meter/meter.c` | 312 | Zero-crossing cycle tracking, true RMS, P, Q, S, PF, phase from the fundamental, frequency, energy, the de-energized floor, and the DC point (Design Report F.M4.2.7). |
| `node/node_app.h` | 60 | Interface of the node application layer. |
| `node/node_app.c` | 137 | Calibration per sample, publication of each window to the registers with the node time, the heartbeat, action on the host's writes, the hand-over of samples and closed cycles to the fault record, the TRIP edge, and STATUS.REC_READY. |
| `capture/capture.h` | 130 | Interface of the fault record, its sizes, and the provisional word stream of a record (layout version 2: float32 row values). |
| `capture/capture.c` | 323 | Fault record (CAP-M4-10): rings of per-cycle rows of every point and of raw samples of one point while armed; the record of 0.2 s before to 2.0 s after the TRIP edge, held until disarmed; the record as 16-bit words for a paged read. |
| `icd4/icd4.h` | 108 | Fields of the two ICD-4 frames and the interface of the link. |
| `icd4/icd4.c` | 182 | Encoding and decoding of the 32-byte controller frame and the 26-byte node frame, the changes between two controller frames, and link supervision (stale after 300 ms, lost frames by sequence number). |

### firmware/board/nucleo_g474re/

| File | Lines | Content |
| --- | --- | --- |
| `README.md` | 143 | State of the layer, build and programming, the design as implemented, the decisions taken in it, and what is not included. |
| `BRINGUP.md` | 90 | Six-stage bring-up sequence for the board, with the expected result of each step. |
| `board.h` | 126 | Node identity, clock and sampling constants, nominal scaling, interrupt priorities, and module interfaces. |
| `board_lock.h` | 27 | Register lock, forced into every source file of the board build. |
| `main.c` | 213 | Start-up order, register lock, millisecond tick, main loop, console banner and report, and fault handler. |
| `clock.c` | 76 | 170 MHz from the 24 MHz crystal, with fallback to the internal oscillator. |
| `sampling.c` | 246 | Pair table of the node; TIM6, ADC1 and ADC2 in dual regular simultaneous mode, and DMA; 16-conversion oversampling; mid-rail tracking; and scaling to volts and amperes. |
| `rs485.c` | 178 | USART3 half-duplex driver: reception by interrupt, frame end by receiver timeout at 1.75 ms, driver enable on PB5. |
| `nv.c` | 411 | Flash store: two 8 KB sectors at the end of bank 2, 96-byte records with CRC-32, background erase, and restore at start-up. |
| `dbg.c` | 124 | Console on LPUART1 (ST-LINK virtual COM port), interrupt-driven ring, output only. |
| `stubs.c` | 19 | Four unused system calls of the C library. |
| `stm32g474re.ld` | 123 | Linker script: program in flash bank 1, 96 KB of RAM, store region left unplaced. |
| `Makefile` | 108 | Build of one node image or all five, and programming by four tools. |
| `vendor/` | n/a | Unmodified ST and Arm files with their licenses: the STM32G474 device header, system file, and start-up file; fourteen low-layer driver headers; and the CMSIS core headers for the Cortex-M4. |
| `prebuilt/` | n/a | The five node images (`m4node_SRC.bin`, `m4node_SY1.bin` to `m4node_SY4.bin`) built with GCC 13.2.1, and `SHA256SUMS`. |

### sim/

| File | Lines | Content |
| --- | --- | --- |
| `simnode.c` | 340 | NODE-SRC, NODE-SY1 (up to five nodes), and the load control unit on two virtual serial ports, running the firmware libraries in real time on the two test circuits of the testing plan. Commands on standard input set the loads, power factor, frequency, the MP-03 input, and take a slave offline. |

### host/

| File | Lines | Content |
| --- | --- | --- |
| `config.yaml` | 83 | Ports, nodes polled, poll and historian timing, alarm bands, derived-quantity inputs, load-control timing, the shed sequence and its settings, the load cards, and the API address. |
| `sgtb_modbus.py` | 144 | Modbus RTU master and tag decoder, standard library only; for the simulator's ports and real serial ports. |
| `poller.py` | 506 | The host loop: 10 Hz polling of both segments, the 1 Hz time broadcast, the live tier with quality and last-good time, derived quantities, alarms including the load fault rules, load-control commands with acknowledgment and confirmation, and queued node writes. |
| `demand.py` | 160 | Demand controller: automatic shedding against the limit, dwell between steps, restoration in reverse after the margin, hold on stale data, and the operator's next, restore-last, and single-contact commands (CAP-M4-38). |
| `api.py` | 506 | Supervisory API (ICD-3): login, roles, sessions, and lockout; snapshot and the 10 Hz live stream; balance, metering, load cards, trends, and CSV export; alarm acknowledgment; shed commands; automatic-control switch; settings; users; and the audit of every call. |
| `historian.py` | 201 | SQLite store in write-ahead-log mode: samples, trends, state changes, commands, alarms, audit, users, settings, the monotonic-time guard, CSV export, and the one-file snapshot. |
| `run_host.py` | 71 | Runs the poller, the historian, and the API server, with one status line per second. |
| `poll_demo.py` | 88 | Live table of every node tag, without the historian. |

### tests/

| File | Lines | Checks | Content |
| --- | --- | --- | --- |
| `test_modbus.c` | 256 | 58 | Modbus slave and both register banks. |
| `test_meter.c` | 327 | 75 | Measurement math against synthetic 3840 Hz inputs, with converter quantization and noise, and the full chain into the registers. |
| `test_capture.c` | 271 | 95 | Fault record: window bounds, row order and values, clip content, the word stream by paged reads with float32 row values, hold and discard, an early trip, clamping, a row value that is not finite; then through the node application with a simulated fault, the trip time stamp, and the control and status bits. |
| `test_icd4.c` | 91 | 61 | Both frames byte by byte against the tables, round trip, length and CRC rejection of every corrupted byte, change detection, lost frames, sequence wrap, and the stale timer. |
| `mbsim.c` | 79 | n/a | Frame-level simulated slave used by `crosscheck.py`. |
| `emu_nucleo.py` | 605 | 109 | The NODE-SRC and NODE-SY1 images on the Unicorn Cortex-M4 emulator with a register-level peripheral model: start-up register values, the measurement chain from converter codes to Modbus registers, bus behaviour, the flash store across restarts and a sector change. Run with `make emu`; not part of `make test`. |
| `crosscheck.py` | 139 | 131 | Independent Python master reads every generated tag from the C slave. |
| `test_simnode.py` | 166 | 39 | Python master against the simulator over virtual ports, in real time. |
| `test_host.py` | 201 | 37 | Poller and historian against the simulator: timing, alarms, the firmware version check, node loss, commands, restart, and exports. |
| `test_api.py` | 268 | 55 | API and demand controller against the simulator: roles, live stream, commands, energy reset, settings, automatic shedding and restoration, session timeout, lockout, and the audit trail. |

## 4. Test cases and the code that serves them

Cases are those of the Module 4 section of the Proof of Concept Mini-Proposal
and Testing Plan (Rev. 07, Table 9).

| Case | Subject | Code in this package | Still to be written |
| --- | --- | --- | --- |
| 1 | Inspection and power-up | `node_app` (valid data on start); board `main.c` (banner, heartbeat lamp) | Boot self-test on the board. |
| 2 | Load command and acknowledgment | `api.py` (`POST /api/commands/shed`), `demand.py`, `poller.py`, `lcu_regs.h`, `modbus_slave` | Load controller sketch on the spare board. |
| 3 | Metering accuracy | `meter`, `node_regs`; board `sampling.c` and `nv.c` (not yet run on hardware) | None; calibration of every channel on the bench. |
| 4 | Frequency and source alerts | `meter` (frequency), `poller.py` (alarms) | Source-position input (IF-03). |
| 5 | Line drop and loss | `poller.py` (derived quantities) | None on the host side. |
| 6 | Live data and generation versus consumption | `api.py` (`/api/live`, `/api/balance`), `poller.py` (live tier, derived quantities) | Scripted ICD-3 client that logs the stream. |
| 7 | Automatic load shedding | `demand.py`, `api.py` (`PUT /api/control/auto`, `PUT /api/config/{key}`) | None on the host side. |
| 8 | Fault record and historian | `capture` (record in the node), `icd4` (frames and link), `node_app` (TRIP edge, REC_ARM, REC_READY), `historian.py` (store, CSV export) | Read of the record over the fieldbus (D-28); TRIP input and controller link on the board; event and capture tables and endpoints. |
| 9 | Fail-safe operation | `poller.py` (node loss, stale marking), `meter` (energy); board `nv.c` (energy and calibration in flash, not yet run on hardware); `capture` (the record is held in RAM until disarmed) | None in the portable code; a record does not survive a loss of node power. |
| 10 | Authorized access | `api.py` (login, roles, session timeout, audit), `historian.py` | None on the host side. |
| 11 | Third-party access and handover | `crosscheck.py`, `historian.py` (`snapshot`), `config.yaml` (node list) | Conformance harness as a stand-alone script. |

## 5. Remaining work, in order

The actions are tracked in the open points list under the IDs shown.

| Order | Work | OPL action | Needs |
| --- | --- | --- | --- |
| 1 | Package committed to the team repository. | A-01 | None. |
| 2 | Load controller files handed to Module 3. | A-02 | None. |
| 3 | Trip capture logic and ICD-4 frame codec, portable C, tested off-target. Done in step 6. Remaining under this action: the read of the record over the fieldbus. | A-03 | D-28 (register layout of the record). |
| 4 | Event and capture tables in the historian and the events endpoints in the API. | A-04 | D-28 for the read of a record from a node; the tables and endpoints themselves need nothing. |
| 5 | Testing plan Case 7 revised; register addresses and expected values of the plan brought to register map 0.2; plan circulated and approved by the technical advisor. | A-05 | D-39. |
| 6 | Stand-in load controller sketch. | A-06 | D-23 (the board). |
| 7 | Conformance harness and scripted ICD-3 client as stand-alone scripts. | A-07 | None. |
| 8 | Node firmware on the NUCLEO-G474RE. Written and emulated: sampling engine, UART framing, and flash storage. Remaining: bring-up on the board (`BRINGUP.md`), self-test and watchdog, the TRIP input on PB4 (a call of `node_app_trip` from the edge interrupt), the controller link on USART1 (`icd4`), detection of a converter channel over range (STATUS.OVERRANGE is reserved and not yet set), and the source-position input. | A-08 | The boards. |
| 9 | Nodes and test circuits built; every channel calibrated. | A-09 | A-08; D-21 and D-22 (source and resistors). |
