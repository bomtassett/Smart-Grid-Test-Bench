# Smart Grid Test Bench (SGTB)

A low-voltage benchtop power grid with dual-source generation, islanded fault protection
and self-healing restoration (FLISR), automatic load shedding, and an end-to-end custom-designed
SCADA stack.

**The Citadel, Department of Electrical and Computer Engineering.**

Senior design project, academic year 2026-27.

## Overview

The bench is a low-voltage model of an electric grid designed for teaching and
demonstration. A wall source and an emulated solar source feed four transmission
branches, each with a switchyard that independently detects a fault, isolates it, and
restores service from a neighboring branch. Four load modules represent six
Charleston-area customers and are shed in a fixed order when demand exceeds
supply. A supervisory system meters the bench, records its history, and gives
an operator visibility and control through a browser.

The bench is built as four modules.

| Module | Title |
| --- | --- |
| M1 | Generation and Conversion |
| M2 | Transmission; Faults, Protection, and Restoration |
| M3 | Distribution and Load |
| M4 | Supervisory Control and Data Acquisition (SCADA) |

![System block diagram of the Smart Grid Test Bench](docs/system-block-diagram.svg)

Black lines are power, red lines are operator switches and indicator lights,
blue lines are data and commands, and orange lines are measurements.

## Repository Layout

Organized by Module:

| Path | Content |
| --- | --- |
| `Module 4 - SCADA/` | Module 4 firmware and host software. |
| `Module 4 - SCADA/regmap/` | `regmap.yaml`, master register map control file |
| `Module 4 - SCADA/tools/` | `gen_regmap.py` validates master register map and generates register artifacts. |
| `Module 4 - SCADA/generated/` | C headers, tag table for the host, and register tables for interface documents. Generated; not edited. |
| `Module 4 - SCADA/firmware/` | Portable C libraries (Modbus RTU slave, register banks, measurement math, fault record, controller link) and board firmware for the NUCLEO-G474RE node controller. |
| `Module 4 - SCADA/host/` | Host service in Python: fieldbus poller, alarms, demand controller, SQLite historian, and supervisory API. |
| `Module 4 - SCADA/sim/` | Simulated nodes and load control unit on virtual serial ports. |
| `Module 4 - SCADA/tests/` | Unit tests, end-to-end tests, and an emulated run of the board firmware. |
| `docs/` | System block diagram and the [Module 4 firmware diagrams](docs/m4_firmware-diagrams.pdf): flowcharts and plain-language walkthroughs of the firmware and host software. |

The folders for Modules 1 to 3 each hold a status page. Their design is recorded in the Design Report,
and module files are added as each module is built.

## Module 4 Software

Builds and tests on a PC; no bench hardware needed. Run commands from `Module 4 - SCADA/`.

| Command | Result |
| --- | --- |
| `make test` | Regenerates and checks master register map, and then runs unit tests, cross-checks, and end-to-end tests. |
| `make sim` | Runs simulated nodes and load control unit. |
| `make host` | Runs the poller, historian, and API against the simulator or the bench. API documentation planned. |
| `make fw` | Builds the five node images for the NUCLEO-G474RE. |
| `make emu` | Runs node images on a Cortex-M4 emulator. |
| `python3 tools/gen_regmap.py` | Regenerates `generated/` after any edit to `regmap/regmap.yaml`. |

Requirements:

- Linux, `make`, and a C99 compiler.
- Python 3 with packages listed in `Module 4 - SCADA/requirements.txt`.
- `arm-none-eabi-gcc` (for `make fw` only).

### Design Overview

- **Measurement.** Five nodes sample voltage and current at 3840 Hz and
  compute RMS voltage and current, active, reactive, and apparent power, power
  factor, frequency, and energy for each cycle. Each node connects to Module 4's [custom
  sensing front-end circuitry](docs/m4_sensor-design_v1.2.pdf).
- **Fieldbus.** The host polls every node at 10 Hz over Modbus RTU on RS-485.
- **Fault record.** During a protection trip the node freezes its per-cycle results
  from 0.2 s before to 2.0 s after the trip, with a clip of raw samples.
- **Load control.** The host commands eight load relays through a load
  control unit on a second RS-485 segment, and sheds and restores load
  automatically against a demand limit table.
- **Historian.** SQLite database holds 1 Hz samples for 24 hours, and
  one-minute trends, state changes, commands, alarms, and audit trail for
  60 days.
- **Supervisory API.** The HMI reads live values and sends commands through
  one API with sessions, three roles, and an audit entry for every change.

## Interface Documents

All inter-module interactions are controlled through six internal interface documents,
issued and maintained by Module 4. Interface docs not included; see Contact to request.

| Document | Title |
| --- | --- |
| ICD-1 | Node Electrical and Mechanical Interface |
| ICD-2 | Data Point Dictionary and Supervisory Register Map |
| ICD-3 | Supervisory API and Data Model |
| ICD-4 | Status and Data Link, Switchyard Controller to Measurement Node |
| ICD-5 | Historian Schema and Export |
| ICD-6 | Load Control Register Map, Host to LCU |


## Status

As of November 22, 2026:

- Design phase is complete; Design Report issued on
  November 3, 2026. Not included.
- Module 4's portable code passes 420 checks in seven test suites and 131
  register readbacks on PC.
- Module 4's board firmware compiles and passes 109 checks on an emulated
  board. Hardware implementation is pending component delivery.
- Proof-of-concept testing scheduled for early December 2026; module
  integration and testing in Spring 2027, with final bench delivery slated
  for early-April 2027.

## Contact

tbassett@student.citadel.edu
Thomas Bassett, Module 4 & Project Manager
