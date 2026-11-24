# THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
# Engineer:  Thomas Bassett
# Created:   2026-11-14
# Modified:  2026-11-24
#
# Project:   Smart Grid Test Bench
# Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
# Level:     1C to 2G - Local Controllers to Applications (build)
# File:      Top-level Build.
#
# Purpose:   One place from which register map files are generated, every test is run, simulator
#            and host service are started, and node firmware is built.
#
# Usage:     make test   generator check + seven PC suites + register cross-check
#            make sim    simulated nodes are started
#            make host   host service is started
#            make fw     five node images are built (arm-none-eabi-gcc needed)
#            make emu    node images are run on Cortex-M4 emulator (arm-none-eabi-gcc; pip install unicorn numpy)

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -Wconversion -Werror -O1 -g
INC      = -Igenerated -Ifirmware/lib/modbus_rtu -Ifirmware/lib/regs -Ifirmware/lib/meter -Ifirmware/lib/node -Ifirmware/lib/capture -Ifirmware/lib/icd4
LIB      = firmware/lib/modbus_rtu/modbus_slave.c firmware/lib/regs/node_regs.c firmware/lib/meter/meter.c firmware/lib/node/node_app.c \
           firmware/lib/capture/capture.c firmware/lib/icd4/icd4.c
HDR      = $(wildcard firmware/lib/*/*.h)
GEN      = generated/regmap_node.h generated/regmap_lcu.h generated/tags.json generated/register_map.md

all: test

$(GEN): regmap/regmap.yaml tools/gen_regmap.py
	python3 tools/gen_regmap.py

build/test_modbus: tests/test_modbus.c $(LIB) $(GEN) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_modbus.c $(LIB) -lm -o $@

build/test_meter: tests/test_meter.c $(LIB) $(GEN) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_meter.c $(LIB) -lm -o $@

build/test_capture: tests/test_capture.c $(LIB) $(GEN) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_capture.c $(LIB) -lm -o $@

build/test_icd4: tests/test_icd4.c $(LIB) $(GEN) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/test_icd4.c $(LIB) -lm -o $@

build/mbsim: tests/mbsim.c $(LIB) $(GEN) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tests/mbsim.c $(LIB) -lm -o $@

build/simnode: sim/simnode.c $(LIB) $(GEN) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS:-std=c99=-std=gnu99) $(INC) sim/simnode.c $(LIB) -lm -o $@

sim: build/simnode
	./build/simnode

host: 
	python3 host/run_host.py

test: build/test_modbus build/test_meter build/test_capture build/test_icd4 build/mbsim build/simnode
	python3 tools/gen_regmap.py --check
	./build/test_modbus
	./build/test_meter
	./build/test_capture
	./build/test_icd4
	python3 tests/crosscheck.py
	python3 tests/test_simnode.py
	python3 tests/test_host.py
	python3 tests/test_api.py

# node firmware, NUCLEO-G474RE: all five images
fw:
	$(MAKE) -C firmware/board/nucleo_g474re every

# node images on Cortex-M4 emulator
emu:
	python3 tests/emu_nucleo.py

clean:
	rm -rf build firmware/board/nucleo_g474re/build

.PHONY: all test sim host fw emu clean
