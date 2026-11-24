# SGTB Module 4: Directory
T. Bassett | 2026-11-23

Lists every file of the package, the package history, and the test cases with the files that serve them.

## Folders

| Folder | Content |
| --- | --- |
| `regmap/` | Register map source |
| `tools/` | Register generator |
| `generated/` | Generator output |
| `firmware/lib/` | Portable firmware libraries |
| `firmware/board/nucleo_g474re/` | NUCLEO-G474RE node firmware |
| `sim/` | Simulator |
| `host/` | Host service |
| `tests/` | Test suites |

## Files

### Root

| File | Content |
| --- | --- |
| `README.md` | Summary, layout, and commands |
| `DIRECTORY.md` | This document |
| `Makefile` | Build and test targets |
| `requirements.txt` | Python packages |
| `.gitignore` | Git ignore list |

### regmap/ and tools/

| File | Content |
| --- | --- |
| `regmap/regmap.yaml` | Master register map source (CST-M4-14) |
| `tools/gen_regmap.py` | Register generator and validator |

### generated/

| File | Content |
| --- | --- |
| `regmap_node.h` | C header for the node |
| `regmap_lcu.h` | C header for the load control unit |
| `tags.json` | Tag table and polling plan |
| `register_map.md` | Register tables, tag list, and bus budget for ICD-2 and ICD-6 |

### firmware/lib/

| File | Content |
| --- | --- |
| `modbus_rtu/modbus_slave.*` | Frame-level Modbus RTU slave |
| `regs/node_regs.*` | Node register bank |
| `regs/lcu_regs.h` | Load control unit register bank, header-only (passes to M3) |
| `meter/meter.*` | Measurement math per point |
| `node/node_app.*` | Node application |
| `capture/capture.*` | Node fault record (CAP-M4-10) |
| `icd4/icd4.*` | Switchyard controller - node link protocol (ICD-4) |

### firmware/board/nucleo_g474re/

| File | Content |
| --- | --- |
| `README.md` | Board firmware state and build |
| `DESIGN.md` | Board firmware design and bring-up sequence |
| `board.h` | Board constants and module interfaces |
| `board_lock.h` | Register lock |
| `main.c` | Start-up, main loop, console report, and fault handler |
| `clock.c` | System clock |
| `sampling.c` | Sampling engine (TIM6, ADC1 and ADC2 in dual mode, DMA) |
| `rs485.c` | RS-485 driver (USART3) |
| `nv.c` | Flash store |
| `dbg.c` | Console (LPUART1) |
| `stubs.c` | C library system call stubs |
| `stm32g474re.ld` | Linker script |
| `Makefile` | Build and programming |
| `vendor/` | ST and Arm headers, system file, and start-up file |
| `prebuilt/` | Five node images and `SHA256SUMS` |

### sim/

| File | Content |
| --- | --- |
| `simnode.c` | Simulated NODE-SRC, NODE-SY1, and LCU on two virtual serial ports |

### host/

| File | Content |
| --- | --- |
| `config.yaml` | Host configuration |
| `sgtb_modbus.py` | Modbus RTU master and tag decoder |
| `poller.py` | Host poller: nodes, timestamps, live tier, alarms, and load commands |
| `demand.py` | Demand and shedding controller |
| `api.py` | Supervisory API (ICD-3) |
| `historian.py` | SQLite historian |
| `run_host.py` | Runs the poller, historian, and API server |
| `poll_demo.py` | Live 10 Hz node tag table |

### tests/

| File | Content |
| --- | --- |
| `test_modbus.c` | Unit tests of the slave and register banks |
| `test_meter.c` | Unit tests of the measurement math |
| `test_capture.c` | Unit tests of the fault record |
| `test_icd4.c` | Unit tests of the ICD-4 frames and link supervision |
| `mbsim.c` | Frame-level simulated node and LCU |
| `crosscheck.py` | Validates generated tags |
| `test_simnode.py` | End-to-end test through the virtual ports |
| `test_host.py` | Host poller and historian against the simulator |
| `test_api.py` | API and demand controller against the simulator |
| `emu_nucleo.py` | Node images on a Cortex-M4 emulator (`make emu`) |

## Packages

Each package is cumulative, and the latest is current.

| Package | Date | Adds |
| --- | --- | --- |
| `m4fw_step1.zip` | 2026-11-14 | Register map source and generator; Modbus RTU slave; register banks; unit tests and cross-check |
| `m4fw_step2.zip` | 2026-11-14 | Measurement math; node application; simulator; host Modbus master and live table |
| `m4fw_step3.zip` | 2026-11-14 | Host poller and historian; host end-to-end test |
| `m4fw_step4.zip` | 2026-11-14 | Supervisory API; demand controller; API end-to-end test |
| `m4fw_step5.zip` | 2026-11-20 | NUCLEO-G474RE node firmware and prebuilt images; emulated test |
| `m4fw_step6.zip` | 2026-11-21 | Fault record; ICD-4 link protocol; two unit-test suites |
| `m4fw_step7.zip` | 2026-11-22 | Register map 0.2 (float32); firmware version 0.2 |
| `m4fw_step8.zip` | 2026-11-22 | Fault record layout version 2 (float32); source comments revised |
| `m4fw_step9.zip` | 2026-11-22 | Host check of the node firmware version; `regmap.yaml` and `gen_regmap.py` as pushed to the team repository |
| `m4fw_step10.zip` | 2026-11-22 | Source comments revised |
| `m4fw_step11.zip` | 2026-11-22 | Source file headers revised |
| `m4fw_step12.zip` | 2026-11-22 | Source comments revised |
| `m4fw_step13.zip` | 2026-11-23 | `README.md` as revised by the engineer |
| `m4fw_step14.zip` | 2026-11-23 | `DIRECTORY.md` and the board `README.md` revised |
| `m4fw_step15.zip` | 2026-11-23 | Board `DESIGN.md` added in place of `BRINGUP.md`; `README.md` and the board `README.md` shortened |

## Test cases

Cases of the Module 4 section of the Proof of Concept Mini-Proposal and Testing Plan (Rev. 07, Table 9).

| Case | Subject | Files |
| --- | --- | --- |
| 1 | Inspection and power-up | `node/node_app.*`; board `main.c` |
| 2 | Load command and acknowledgment | `host/api.py`, `host/demand.py`, `host/poller.py`, `regs/lcu_regs.h`, `modbus_rtu/modbus_slave.*` |
| 3 | Metering accuracy | `meter/meter.*`, `regs/node_regs.*`; board `sampling.c`, `nv.c` |
| 4 | Frequency and source alerts | `meter/meter.*`, `host/poller.py` |
| 5 | Line drop and loss | `host/poller.py` |
| 6 | Live data and generation versus consumption | `host/api.py`, `host/poller.py` |
| 7 | Automatic load shedding | `host/demand.py`, `host/api.py` |
| 8 | Fault record and historian | `capture/capture.*`, `icd4/icd4.*`, `node/node_app.*`, `host/historian.py` |
| 9 | Fail-safe operation | `host/poller.py`, `meter/meter.*`, `capture/capture.*`; board `nv.c` |
| 10 | Authorized access | `host/api.py`, `host/historian.py` |
| 11 | Third-party access and handover | `tests/crosscheck.py`, `host/historian.py`, `host/config.yaml` |
