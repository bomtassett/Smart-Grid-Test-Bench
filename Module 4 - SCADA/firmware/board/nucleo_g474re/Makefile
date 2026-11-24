# THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
# Engineer:  Thomas Bassett
# Created:   2026-11-20
# Modified:  2026-11-24
#
# Project:   Smart Grid Test Bench
# Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
# Level:     1C - Local Controllers
# File:      Measurement Node Firmware Build, NUCLEO-G474RE.
#
# Purpose:   Builds firmware image of one measurement node, or of all five, and programs it into board.
#
# Usage:     make NODE=SRC        source node (slave address 1, four points)
#            make NODE=SY1        switchyard node 1 (slave address 2, two points); SY2, SY3, SY4 likewise
#            make every           all five images
#            make flash NODE=SY1  board on ST-LINK is programmed (see FLASH_TOOL)
#
# Notes:     Arm-none-eabi-gcc w/ newlib is needed (Debian, Ubuntu: gcc-arm-none-eabi, libnewlib-arm-none-eabi).
#            The ST headers and start-up file are in vendor/.

NODE ?= SRC
ROOT  = ../../..
OUT  ?= build/$(NODE)
EXTRA ?=
NAME  = m4node_$(NODE)

ifeq ($(NODE),SRC)
NODEDEF = -DNODE_SRC
else ifneq ($(filter $(NODE),SY1 SY2 SY3 SY4),)
NODEDEF = -DNODE_SY=$(subst SY,,$(NODE))
else
$(error NODE must be SRC, SY1, SY2, SY3, or SY4)
endif

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

CPU     = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
INC     = -I. -Ivendor/cmsis_core -Ivendor/cmsis_device -Ivendor/ll \
          -I$(ROOT)/generated -I$(ROOT)/firmware/lib/modbus_rtu -I$(ROOT)/firmware/lib/regs \
          -I$(ROOT)/firmware/lib/meter -I$(ROOT)/firmware/lib/node -I$(ROOT)/firmware/lib/capture -I$(ROOT)/firmware/lib/icd4
DEFS    = -DSTM32G474xx -DHSE_VALUE=24000000U $(NODEDEF) $(EXTRA)
# board_lock.h is forced into every file, so that register bank is built w/ lock
CFLAGS  = $(CPU) -std=gnu11 -O2 -g3 -ffunction-sections -fdata-sections -fno-common -fno-math-errno \
          -Wall -Wextra -Werror -Wshadow -Wdouble-promotion -include board_lock.h $(DEFS) $(INC) -MMD -MP
# --no-warn-rwx-segments is known to GNU ld 2.39 and later only; pass it when linker accepts it
NOWARN  = $(shell $$($(CC) -print-prog-name=ld) --no-warn-rwx-segments --version >/dev/null 2>&1 && echo -Wl,--no-warn-rwx-segments)																							   
LDFLAGS = $(CPU) -Tstm32g474re.ld --specs=nano.specs --specs=nosys.specs \
          -Wl,--gc-sections -Wl,-Map=$(OUT)/$(NAME).map $(NOWARN)

BOARD   = main.c clock.c sampling.c rs485.c dbg.c nv.c stubs.c
LIB     = $(ROOT)/firmware/lib/modbus_rtu/modbus_slave.c $(ROOT)/firmware/lib/regs/node_regs.c \
          $(ROOT)/firmware/lib/meter/meter.c $(ROOT)/firmware/lib/node/node_app.c \
          $(ROOT)/firmware/lib/capture/capture.c $(ROOT)/firmware/lib/icd4/icd4.c
VENDOR  = vendor/cmsis_device/system_stm32g4xx.c
STARTUP = vendor/cmsis_device/startup_stm32g474xx.s

OBJ = $(addprefix $(OUT)/,$(notdir $(BOARD:.c=.o) $(LIB:.c=.o) $(VENDOR:.c=.o) $(STARTUP:.s=.o)))
vpath %.c . $(sort $(dir $(LIB))) vendor/cmsis_device
vpath %.s vendor/cmsis_device

all: $(OUT)/$(NAME).bin $(OUT)/$(NAME).hex
	@$(SIZE) $(OUT)/$(NAME).elf

every:
	@for n in SRC SY1 SY2 SY3 SY4; do $(MAKE) --no-print-directory NODE=$$n all || exit 1; done

$(OUT)/%.o: %.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

# doubles are used by portable libs in a few window-rate sums
$(OUT)/meter.o $(OUT)/node_regs.o $(OUT)/capture.o: CFLAGS += -Wno-double-promotion

$(OUT)/%.o: %.s | $(OUT)
	$(CC) $(CPU) -c $< -o $@

$(OUT)/$(NAME).elf: $(OBJ) stm32g474re.ld
	$(CC) $(OBJ) $(LDFLAGS) -lm -o $@

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
%.hex: %.elf
	$(OBJCOPY) -O ihex $< $@

$(OUT):
	@mkdir -p $@

# FLASH_TOOL = stlink  (st-flash, package stlink-tools)
#            = cube    (STM32_Programmer_CLI, from STM32CubeProgrammer)
#            = openocd
#            = drag    (copy to NODE_G474RE mass-storage drive; DRIVE = its mount point)
FLASH_TOOL ?= stlink
DRIVE      ?= /media/$(USER)/NODE_G474RE
flash: $(OUT)/$(NAME).bin
ifeq ($(FLASH_TOOL),stlink)
	st-flash --reset write $< 0x08000000
else ifeq ($(FLASH_TOOL),cube)
	STM32_Programmer_CLI -c port=SWD -w $< 0x08000000 -v -rst
else ifeq ($(FLASH_TOOL),openocd)
	openocd -f interface/stlink.cfg -f target/stm32g4x.cfg -c "program $(OUT)/$(NAME).elf verify reset exit"
else
	cp $< $(DRIVE)/ && sync
endif

clean:
	rm -rf build

-include $(wildcard build/*/*.d)
.PHONY: all every flash clean
