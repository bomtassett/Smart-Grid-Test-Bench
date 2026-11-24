# SGTB Module 4: Firmware and Host Software
T. Bassett | 2026-11-23

The following Module 4 components are ready for proof-of-concept testing:

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

As of 2026-11-22, the software layer has been built and tested successfully on a PC. The board firmware compiles and has passed an emulated run -- however, it has not yet been implemented in hardware (see `firmware/board/nucleo_g474re/README.md`). The component delivery schedule remains the project's largest risk factor in advance of the proof-of-concept testing demonstration on 2026-12-03.

## Layout

| Path | Content |
| --- | --- |
| `regmap/regmap.yaml` | Master register map source (CST-M4-14) |
| `tools/gen_regmap.py` | Register generator and validator |
| `generated/regmap_node.h`, `regmap_lcu.h` | C headers for the node and load control unit |
| `generated/tags.json` | Tag table and polling plan |
| `generated/register_map.md` | Register tables, tag list, and bus budget for ICD-2 and ICD-6 |
| `firmware/lib/modbus_rtu/` | Frame-level Modbus RTU slave |
| `firmware/lib/regs/node_regs.*` | Node register bank |
| `firmware/lib/regs/lcu_regs.h` | Load control unit register bank, header-only (passes to M3) |
| `firmware/lib/meter/` | Measurement math per point (Design Report F.M4.2.7) |
| `firmware/lib/node/node_app.*` | Node application |
| `firmware/lib/capture/` | Node fault record (CAP-M4-10) |
| `firmware/lib/icd4/` | Switchyard controller - node link protocol (ICD-4; provisional pending M2 confirmation) |
| `firmware/board/nucleo_g474re/` | NUCLEO-G474RE node firmware and design document |
| `sim/simnode.c` | Simulated NODE-SRC, NODE-SY1, and LCU on two virtual serial ports |
| `host/sgtb_modbus.py` | Modbus RTU master and tag decoder |
| `host/poll_demo.py` | Live 10 Hz node tag table |
| `host/config.yaml` | Host configuration |
| `host/poller.py` | Host poller: node, timestamps, live tier, alarms, load commands |
| `host/demand.py` | Demand and shedding controller |
| `host/api.py` | Supervisory API (ICD-3) |
| `host/historian.py` | SQLite historian |
| `host/run_host.py` | Runs the poller, historian, and API server |
| `tests/test_modbus.c` | Unit tests of the slave and register banks |
| `tests/test_meter.c` | Unit tests of the measurement math |
| `tests/test_capture.c` | Unit tests of the fault record |
| `tests/test_icd4.c` | Unit tests of the ICD-4 frames and link supervision |
| `tests/test_simnode.py` | End-to-end test through the virtual ports |
| `tests/test_host.py` | Host poller and historian against the simulator |
| `tests/test_api.py` | API and demand controller against the simulator |
| `tests/emu_nucleo.py` | Node images on a Cortex-M4 emulator (`make emu`) |
| `tests/mbsim.c` | Frame-level simulated node and LCU |
| `tests/crosscheck.py` | Validates generated tags |

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

## Node Integration

Integration is handled in `firmware/board/nucleo_g474re/`.

## Miscellaneous

In script header sections, the `Level` line places the file in the ISA112 SCADA System Model Architecture
Diagram (interim working draft of the ISA112 committee, 2022-01-26). It gives the Purdue level shown on that diagram,
then the layer letter and the layer name, to better help developers understand each component's relative position
within standard SCADA architecture:

| Level | Layer | In this package |
| --- | --- | --- |
| 0A, 0B | Field Devices, Field Sensor Networks | Hardware only: dividers and shunts |
| 1C | Local Controllers | Measurement node firmware; simulated nodes |
| 1D | Controller Network | Register map and its generated files (Modbus RTU fieldbus) |
| 2F | Communication Servers | Modbus master and polling on the host |
| 2G | Applications | Alarms and events, historian, demand controller, and supervisory API |
| 3H | Control Network | Not in the package; the supervisory API is the host's port onto it |

A file that serves two layers names both (`2F/2G`). A parenthesis marks a
file that is an interface to another layer, a test, a simulation, or a tool.
