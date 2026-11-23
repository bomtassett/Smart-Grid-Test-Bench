# SGTB Module 4: Firmware and Host Software
T. Bassett | 2026-11-22

Module 4 software ready for proof-of-concept testing:

	* master register-map source
	* register generator
	* portable Modbus RTU slave library
	* measurement node register banks (ICD-2)
	* load control unit register banks (ICD-6)
	* measurement math
	* node application layer
	* simulated node on virtual serial ports
	* host service
	* NUCLEO-G474RE-specific firmware for the measurement node
	
As of 2026-11-22, the software layer has been built and tested successfully on a PC. The board firmware compiles and has passed an emulated run-- however, it has not yet been implemented in hardware (see `firmware/board/nucleo_g474re/README.md` and `BRINGUP.md` there). The team's component delivery schedule remains the largest risk in advance of our proof-of-concept demonstration on 2026-12-03.

## Layout

| Path | Content |
| --- | --- |
| `regmap/regmap.yaml` | Master register-map source (CST-M4-14). The only file edited by hand to change an address, type, or scale. |
| `tools/gen_regmap.py` | Register generator and validator. Rejects overlapping registers, duplicate tags, duplicate slave addresses, and a segment 1 utilization above 50% (CAP-M4-34). |
| `generated/regmap_node.h`, `regmap_lcu.h` | C headers for the node and load control unit. |
| `generated/tags.json` | Tag table and polling plan for the host poller, historian, and conformance harness. |
| `generated/register_map.md` | Register tables, tag list, and bus budget for ICD-2 and ICD-6. |
| `firmware/lib/modbus_rtu/` | Frame-level Modbus RTU slave: FC 03, 04, 06, and 16; exceptions 01 to 03; broadcast writes. C99, no dynamic memory. |
| `firmware/lib/regs/node_regs.*` | Node register bank: float32 measured values as given, with the fault flag for a value that is not finite; latched reads, calibration, time set, energy reset, and control word. |
| `firmware/lib/regs/lcu_regs.h` | Load control unit register bank, header-only (one included for the Module 3 sketch). |
| `firmware/lib/meter/` | Measurement math per point (Design Report F.M4.2.7): zero-crossing cycle tracking, true RMS, P, Q, S, PF, phase by fundamental DFT, frequency, energy, and the de-energized floor. Streaming, no sample buffers. |
| `firmware/lib/node/node_app.*` | Node application: calibration per sample, timestamped windows published to registers, heartbeat, the host's writes acted on, and the fault record fed and armed. |
| `firmware/lib/capture/` | Fault record of a node (CAP-M4-10): per-cycle rows of every point from 0.2 s before to 2.0 s after the TRIP edge, and a raw clip of one point from 50 ms before to 150 ms after. The word stream of a record holds each row value as a float32 (layout version 2) and is provisional (decision D-28). |
| `firmware/lib/icd4/` | Frames of the link between a switchyard controller and its node (ICD-4): 32 bytes from the controller, 26 bytes from the node, and link supervision. Byte order, frame check, and enumeration values are provisional until the Module 2 firmware header is shared. |
| `firmware/board/nucleo_g474re/` | Node firmware for the NUCLEO-G474RE: clock, sampling engine (TIM6, ADC1 and ADC2 in dual mode, DMA), RS-485 driver, flash store, console, and main program, with the ST headers it needs. `README.md` and `BRINGUP.md` in the folder. |
| `sim/simnode.c` | Simulated NODE-SRC, NODE-SY1 (up to five nodes), and the load control unit on two virtual serial ports, running the firmware libraries in real time on the two test circuits of the testing plan. Linux or macOS. |
| `host/sgtb_modbus.py` | Minimal Modbus RTU master and tag decoder, standard library only; works on the simulator's ports and on real serial ports. |
| `host/poll_demo.py` | Live table of every node tag at 10 Hz. |
| `host/config.yaml` | Host configuration: ports, nodes polled, timing, alarm bands, derived-quantity inputs. |
| `host/poller.py` | Host poller: 10 Hz polling of both segments, 1 Hz time broadcast, live tier with quality and last-good time, derived quantities, alarms, the check of each node's firmware version against the register map version, load-control commands with acknowledgment and confirmation. |
| `host/demand.py` | Demand controller: automatic shedding and restoration, and the operator's shed commands. |
| `host/api.py` | Supervisory API (ICD-3): sessions and roles, live stream, metering, trends, alarms, commands, settings, users, exports, and audit. |
| `host/historian.py` | SQLite historian in write-ahead-log mode: 1 Hz samples, trend rows, state changes, commands, alarms, audit, CSV export, and the one-file snapshot. |
| `host/run_host.py` | Runs the poller, historian, and API server. The first start creates the user `admin` and prints its password once. |
| `tests/test_modbus.c` | Off-target unit tests of the slave and register banks (58 checks). |
| `tests/test_meter.c` | Measurement math against synthetic 3840 Hz inputs, with converter quantization and noise (75 checks). |
| `tests/test_capture.c` | Fault record alone and through the node application with a simulated fault (95 checks). |
| `tests/test_icd4.c` | ICD-4 frames and link supervision (61 checks). |
| `tests/test_simnode.py` | End-to-end test through the virtual ports (39 checks). |
| `tests/test_host.py` | Host poller and historian against the simulator (37 checks). |
| `tests/test_api.py` | API and demand controller against the simulator (55 checks). |
| `tests/emu_nucleo.py` | The node images on a Cortex-M4 emulator with a register-level model of the peripherals (109 checks). Run with `make emu`; not part of `make test`. |
| `tests/mbsim.c` | Frame-level simulated node or load control unit (hex frames on stdin and stdout). |
| `tests/crosscheck.py` | Independent Python master that reads every generated tag from the C slave and compares values. |

## Use

    make test                      # regenerate check, unit tests, cross-check, end-to-end
    make sim                       # run the simulator; commands on stdin (see sim/simnode.c)
    python3 host/poll_demo.py      # in a second terminal: live values from /tmp/sgtb-seg1
    make host                      # or: the poller, historian, and API (http://127.0.0.1:8000/docs)
    make fw                        # the five node images for the NUCLEO-G474RE (arm-none-eabi-gcc)
    make emu                       # the node images on the emulator (unicorn, numpy)
    python3 tools/gen_regmap.py    # after any edit to regmap/regmap.yaml
	
See `DIRECTORY.md` for all files, package history, and the map from test cases to code.
    
Requires gcc and Python 3 with the packages in `requirements.txt` (`pip install -r requirements.txt`). `generated/` is committed; `make test`
fails if it is out of date with the YAML source.

## Source commenting standard

Every hand-written source file opens with one header, in the comment syntax of
its language (a module docstring in Python):

    THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
    Engineer:  <name>
    Created:   <date>
    Modified:  <date of the last edit>, with a file version where one is kept

    Project:   Smart Grid Test Bench
    Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
    Level:     <Purdue level and ISA112 layer> - <layer name>
    File:      <title of the file> (<requirement or interface document reference>).

    Purpose:   <what the file is for, in sentences a reader outside the project can follow>

    Output:    <files produced, where there are any>

    Usage:     <command line, where there is one>

    Notes:     <the terms the file relies on, each defined before it is used>

The `Level` line places the file in the ISA112 SCADA System Model Architecture
Diagram (interim working draft of the ISA112 committee, 2022-01-26), which is
the reference of the project. It gives the Purdue level shown on that diagram,
then the layer letter and the layer name:

| Level | Layer | In this package |
| --- | --- | --- |
| 0A, 0B | Field Devices, Field Sensor Networks | Hardware only: dividers and shunts. |
| 1C | Local Controllers | Measurement node firmware; simulated nodes. |
| 1D | Controller Network | Register map and its generated files (Modbus RTU fieldbus). |
| 2F | Communication Servers | Modbus master and polling on the host. |
| 2G | Applications | Alarms and events, historian, demand controller, and supervisory API. |
| 3H | Control Network | Not in the package; the supervisory API is the host's port onto it. |

A file that serves two layers names both (`2F/2G`). A parenthesis marks a
file that is an interface to another layer, a test, a simulation, or a tool.

## Integration on the node

Integration is handled in `firmware/board/nucleo_g474re/`, with a UART driver delimiting
frames with 1.75 ms of inactivity. The main loop calls

    n = mb_slave_process(&slave, rx, rx_len, tx, sizeof tx, &stats);

sending `tx[0..n)` when `n` is not zero. `NODE_REGS_LOCK` and
`NODE_REGS_UNLOCK` are defined in `board_lock.h` to mask the sampling
interrupt around the live-to-latched copy.
