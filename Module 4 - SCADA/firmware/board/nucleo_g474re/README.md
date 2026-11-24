# SGTB Module 4: NUCLEO-G474RE Node Firmware
T. Bassett | 2026-11-23

Board-specific layer of the measurement node: clock, sampling engine, RS-485
driver, flash store, console, and main program.

**State.** The five images compile without warnings and pass an emulated run
(`tests/emu_nucleo.py`, 109 checks). Nothing in this folder has run on a board.
`DESIGN.md` gives the design as implemented and the bring-up sequence.

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
