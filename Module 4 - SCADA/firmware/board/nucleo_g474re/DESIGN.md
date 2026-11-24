# SGTB Module 4: NUCLEO-G474RE Node Firmware Design
T. Bassett | 2026-11-24

This folder is the board-specific layer of the measurement node: clock, sampling
engine, RS-485 driver, flash store, console, and main program. The
measurement math, register bank, and Modbus slave are the portable
libraries of `firmware/lib/`, unchanged; the simulator and this firmware run
the same code above the hardware.

## Design as implemented

Pins, peripherals, and rates follow Module 4: Sensor Design, revision 1.2, and
Design Report Table F.M4.15. Table F.M4.15 lists the current input of pair 1
(PA1) as ADC1_IN2; it is converted on ADC2 (ADC2_IN2), as dual simultaneous
mode requires.

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
counts (8192 = 1.65 V). `samples` increments 3840 times per second. `overruns` counts
sampling interrupts served late and is to stay at zero. `mb ok` counts frames
answered; `other`, frames for another address; `crc`, `exc`, and `line`, CRC
errors, exception replies, and frames dropped for a line error. `status` is
the health register.

## Decisions taken in this layer

None are yet bound by design documents and are open to change.

| Decision | Basis |
| --- | --- |
| Pair p of the front end is point p of the register map. On NODE-SRC: MP-01, MP-02, MP-03, MP-04 on pairs 1 to 4. On a switchyard node: the 24 V side on pair 1 and the load feed on pair 2. | Order of the register map; the wiring list is to confirm it. |
| The DC point MP-02 uses the 12 V divider and the 0.01 ohm shunt, centered on mid-rail. | Placeholder until Module 1 fixes the divider and shunt; one line of the pair table in `sampling.c`. |
| Node identity at build time rather than by straps. | No strap pins are assigned in Table F.M4.15. |
| PC8 as a sample-rate test pin and PA5 (LD2) as heartbeat. | Free pins; either is removed by deleting one line. |
| Calibration reported valid (status bit 1) only after constants are restored from flash or written by the host. | An uncalibrated node is then visible to the host. |
| The store is disabled when the flash is not in the dual-bank layout. | In dual-bank layout a write in bank 2 does not stall the program in bank 1; in single-bank layout an erase would stop the sampling interrupt for about 22 ms. Dual bank is the factory state. |
| No correction for the 16-conversion average. | It lowers a 60 Hz reading by 0.04%; the gain calibration at 60 Hz absorbs it. |

## Functions not yet implemented

Trip input (PB4), the ICD-4 link on USART1, the source-position input, the
boot self-test, and the watchdog. PB4, PC4, and PC5 are left in their reset
state. The fault record (`firmware/lib/capture`) is built in and armed from
start-up, and the frames of the link are in `firmware/lib/icd4`; neither is
connected to a pin yet. The record is triggered by a call of `node_app_trip`
from the edge interrupt of PB4 once the polarity of TRIP is confirmed with
Module 2. The record adds about 18.5 KB of RAM.

Two items of the sensor design (Section 11) are also not implemented: the
stored phase correction of each voltage-current pair, and the calibration date
in the store record. The record holds the gain and offset of each channel, the
energy totals, and the node address.

## Emulated test

`python3 tests/emu_nucleo.py` (or `make emu` at the package root) runs the
real images on the Unicorn Cortex-M4 emulator with a register-level model of
the peripherals. It checks the registers programmed at start-up against values
worked out independently, feeds synthetic converter codes through the sampling
interrupt, exchanges Modbus frames through the USART3 interrupt, and restarts
the image on the flash contents it left. In that run the sampling interrupt
took a median of about 1100 instructions on NODE-SRC and at most about 16 200
(a window closing on all four points in one interrupt), against a period of
44 272 processor cycles.

The emulator models register behavior only: no analog behavior, no timing
inside a peripheral, and no pre-emption of one interrupt by another. A pass is
evidence about the logic and the configuration values, not about the board.

## Bring-up sequence

The firmware has passed an emulated run only. The stages below confirm, in
order of dependence, the behavior that the emulator cannot show. Each stage
needs only what has been confirmed by earlier stages. Stages 1 to 4 need the
bare NUCLEO-G474RE and bench instruments; no front-end board is required.

Console: ST-LINK virtual COM port (`/dev/ttyACM0` on Linux), 115 200
bit/s, 8N1, for example `picocom -b 115200 /dev/ttyACM0`.

Every converter input is limited to the range 0 V to 3.3 V. A signal source is
to be set, and checked on an oscilloscope, before it is connected to a pin.

### Stage 1: program, clock, and console

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 1.1 | Connect the board by USB only. Program `m4node_SY1.bin`. | Console banner: `NODE-SY1  address 2`. | No text: confirm the port, and that the 5 V source jumper (JP5 in UM2505) selects the ST-LINK supply. |
| 1.2 | Read the clock line of the banner. | `clock: 24 MHz crystal`; LD2 blinks at 1 Hz. | `INTERNAL OSCILLATOR` and a 5 Hz blink: the crystal X3 did not start. Check solder bridges SB25 and SB26 against UM2505. Frequency accuracy is not met until this is resolved. |
| 1.3 | Read the store line of the banner. | `no calibration stored, nominal scaling in use, energy starts at zero`. | `DISABLED`: the option bit DBANK is clear; set it with STM32CubeProgrammer. |
| 1.4 | Watch the status line for 10 s. | `samples` rises by 3840 per line; `overruns 0`. | `samples` fixed at zero: the trigger timer, the converters, or the DMA channel is not running; the fault is in `sampling_init()` or `sampling_start()`. |

### Stage 2: sampling rate

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 2.1 | Measure PC8 (CN10 pin 2) with the counter of the oscilloscope or the 34401A in frequency mode. | 1919.95 Hz, within the 20 ppm of the crystal. | Half or double: the trigger divider or the oversampling count is wrong. A wandering value with the internal oscillator in use is expected. |
| 2.2 | Observe PC8 on the oscilloscope with infinite persistence for 1 min. | Edge spread well under one period (260 us); a few tens of microseconds each 100 ms, when a window closes. | Edges missing or `overruns` above zero: the interrupt is running long. Record the worst spacing. |

### Stage 3: converter inputs and scaling

A function generator set to 60 Hz sine, 1.000 V peak to peak, with 1.650 V
offset, into a high-impedance load, gives 0.3536 V RMS at the pin.

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 3.1 | With nothing connected, read the `mid` values. | Unsteady values; open inputs float. No conclusion is drawn. | n/a |
| 3.2 | Apply the generator to PA0 (A0, CN7 pin 28). | NODE-SY1: `p0 V` near 10.96 (0.3536 x 31); `f 60.000` within the accuracy of the generator; first `mid` value near 8192. | Reading on the wrong point or on a current column: the channel order of the pair table is wrong. |
| 3.3 | Move the generator to PA1 (A1, CN7 pin 30). | `p0 I` near 0.3536 (0.3536 / (20 x 0.05)). | As 3.2. |
| 3.4 | Repeat on PA4 (A2, CN7 pin 32) and PB0 (A3, CN7 pin 34). | `p1 V` near 5.657 (0.3536 x 16); `p1 I` near 0.3536. | As 3.2. This pair has voltage on ADC2 and current on ADC1; a swap shows here. |
| 3.5 | Apply the generator to PA0 and PA1 together. | `p0 pf 1.000`; `P` equal to V x I within 0.5%. | A power factor below 1 with one source on both pins means the two converters are not sampling together. |
| 3.6 | NODE-SRC image only: repeat on PC0 (A5, CN7 pin 38), PC1 (A4, CN7 pin 36), PC2 (CN7 pin 35), and PC3 (CN7 pin 37). | `p2 V` near 5.657, `p2 I` near 1.768; `p3 V` near 10.96, `p3 I` near 0.884. | As 3.2. |

The Arduino labels (A0 to A5) and the CN7 and CN10 pin numbers are those of
UM2505 Rev 7, Tables 15 and 16. CN7 pins 36 and 38 are shared (PC1/PB9 and
PC0/PA15); the selection, and the silkscreen, are to be checked on the board
before a source is connected.

### Stage 4: fieldbus

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 4.1 | Connect a 3.3 V USB serial adapter directly: adapter TX to PB11, adapter RX to PB10, and ground. Run `python3 host/poll_demo.py /dev/ttyUSB0 NODE-SY1`. | Live table of NODE-SY1; `mb ok` on the console rises steadily; `crc 0`, `line 0`. | No reply and `mb ok` at zero with `other` or `crc` rising: address or line settings. Nothing rising: wiring, or the receiver timeout is not ending frames. |
| 4.2 | Observe PB5 and PB10 on the oscilloscope during a poll. | PB5 high from before the first start bit until after the last stop bit of the reply, and low otherwise. Reply begins about 2 ms after the request ends (wait 1.75 ms, then the main loop). | PB5 falling before the last stop bit truncates replies on the transceiver. |
| 4.3 | Fit the RS-485 transceiver and repeat 4.1 through a USB RS-485 adapter. | As 4.1. | Replies seen by the oscilloscope but CRC errors on the host: an adapter that echoes its own transmission; the host master does not discard an echo. |
| 4.4 | Run `make host` with the node list of `host/config.yaml` reduced to the nodes present. | Node shown online; node time follows the 1 Hz broadcast (`t` on the console steps to the host's value). | n/a |

### Stage 5: store

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 5.1 | Write a gain of 1.01 to channel 0 (HR 110 to HR 113) from the host. | Bit 1 of the status word is set (`status 0x0003`) and `p0 V` rises by 1%; 2 s later, the console prints `store: record written`. `overruns` holds at 0. | `overruns` = 1 at the write: flash write stalled the program because the dual-bank assumption is incorrect. |
| 5.2 | Remove and restore power. | Banner: `calibration restored, energy restored`; `p0 V` still scaled. | `no calibration stored`: the record was not written or not found. |
| 5.3 | Leave a point energized for 10 min and cycle power. | Energy within 60 s of accumulation of pre-cycle value. | n/a |

To write gain for 5.1:

    python3 - <<'PY'
    import struct, sys
    sys.path.insert(0, "host")
    from sgtb_modbus import Master
    m = Master("/dev/ttyUSB0")
    words = struct.unpack(">4H", struct.pack(">ff", 1.01, 0.0))   # gain, offset
    m.write_multiple(2, 110, list(words))                         # slave 2 is NODE-SY1
    PY

### Stage 6: front end attached

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 6.1 | With the front end powered and no signal, read `mid`. | Both values of each pair within 8192 +/- 100 (1.65 V +/- 20 mV). | A value near 0 or 16383: V_MID is missing or a channel is saturated. |
| 6.2 | Proceed to the calibration and verification steps of the testing plan (Rev. 07, Case 3). | n/a | n/a |

### Decision dependencies

The results of 1.2 (crystal), 2.1 and 2.2 (rate and interrupt length), 3.4 and
3.5 (pair order and simultaneity), 4.2 (driver-enable timing), and 5.1 (stall
on a flash write) decide whether any of the decisions listed under "Decisions taken in this layer"
has to change.