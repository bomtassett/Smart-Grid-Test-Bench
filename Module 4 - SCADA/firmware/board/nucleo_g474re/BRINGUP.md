# Bring-up sequence for the node firmware

SGTB Module 4. Issued November 20, 2026, with package `m4fw_step5.zip`.

The firmware has passed an emulated run only. The stages below confirm, in
order of dependence, the behaviour that the emulator cannot show. Each stage
needs only what the stages before it have confirmed. Stages 1 to 4 need the
bare NUCLEO-G474RE and bench instruments; no front-end board is required.

Console: the ST-LINK virtual COM port (`/dev/ttyACM0` on Linux), 115 200
bit/s, 8N1, for example `picocom -b 115200 /dev/ttyACM0`.

Every converter input is limited to the range 0 V to 3.3 V. A signal source is
to be set, and checked on an oscilloscope, before it is connected to a pin.

## Stage 1: program, clock, and console

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 1.1 | Connect the board by USB only. Program `m4node_SY1.bin`. | Console banner: `NODE-SY1  address 2`. | No text: confirm the port, and that the 5 V source jumper (JP5 in UM2505) selects the ST-LINK supply. |
| 1.2 | Read the clock line of the banner. | `clock: 24 MHz crystal`; LD2 blinks at 1 Hz. | `INTERNAL OSCILLATOR` and a 5 Hz blink: the crystal X3 did not start. Check solder bridges SB25 and SB26 against UM2505. Frequency accuracy is not met until this is resolved. |
| 1.3 | Read the store line of the banner. | `no calibration stored, nominal scaling in use, energy starts at zero`. | `DISABLED`: the option bit DBANK is clear; set it with STM32CubeProgrammer. |
| 1.4 | Watch the status line for 10 s. | `samples` rises by 3840 per line; `overruns 0`. | `samples` fixed at zero: the trigger timer, the converters, or the DMA channel is not running; the fault is in `sampling_init()` or `sampling_start()`. |

## Stage 2: sampling rate

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 2.1 | Measure PC8 (CN10 pin 2) with the counter of the oscilloscope or the 34401A in frequency mode. | 1919.95 Hz, within the 20 ppm of the crystal. | Half or double: the trigger divider or the oversampling count is wrong. A wandering value with the internal oscillator in use is expected. |
| 2.2 | Observe PC8 on the oscilloscope with infinite persistence for 1 min. | Edge spread well under one period (260 us); a few tens of microseconds each 100 ms, when a window closes. | Edges missing or `overruns` above zero: the interrupt is running long. Record the worst spacing. |

## Stage 3: converter inputs and scaling

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

The Arduino labels (A0 to A5) and the CN7 pin numbers are those of the
NUCLEO-64 pinout as recalled from UM2505; they are to be confirmed against
the manual and the board silkscreen before a source is connected.

## Stage 4: fieldbus

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 4.1 | Connect a 3.3 V USB serial adapter directly: adapter TX to PB11, adapter RX to PB10, and ground. Run `python3 host/poll_demo.py /dev/ttyUSB0 NODE-SY1`. | Live table of NODE-SY1; `mb ok` on the console rises steadily; `crc 0`, `line 0`. | No reply and `mb ok` at zero with `other` or `crc` rising: address or line settings. Nothing rising: wiring, or the receiver timeout is not ending frames. |
| 4.2 | Observe PB5 and PB10 on the oscilloscope during a poll. | PB5 high from before the first start bit until after the last stop bit of the reply, and low otherwise. Reply begins about 2 ms after the request ends (1.75 ms of silence, then the main loop). | PB5 falling before the last stop bit truncates replies on the transceiver. |
| 4.3 | Fit the RS-485 transceiver and repeat 4.1 through a USB RS-485 adapter. | As 4.1. | Replies seen by the oscilloscope but CRC errors on the host: an adapter that echoes its own transmission; the host master does not discard an echo. |
| 4.4 | Run `make host` with the node list of `host/config.yaml` reduced to the nodes present. | Node shown online; node time follows the 1 Hz broadcast (`t` on the console steps to the host's value). | n/a |

## Stage 5: store

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 5.1 | Write a gain of 1.01 to channel 0 (holding registers 110 to 113) from the host. | The status word shows bit 1 at once (`status 0x0003`) and `p0 V` rises by 1%; 2 s later the console prints `store: record written`. `overruns` stays at zero. | `overruns` stepping at the moment of the write: the flash write stalled the program; report it, as the dual-bank assumption is then wrong. |
| 5.2 | Remove and restore power. | Banner: `calibration restored, energy restored`; `p0 V` still scaled. | `no calibration stored`: the record was not written or not found. |
| 5.3 | Leave a point energized for 10 min and cycle power. | Energy within 60 s of accumulation of the value before the cycle. | n/a |

One way to make the write of 5.1:

    python3 - <<'PY'
    import struct, sys
    sys.path.insert(0, "host")
    from sgtb_modbus import Master
    m = Master("/dev/ttyUSB0")
    words = struct.unpack(">4H", struct.pack(">ff", 1.01, 0.0))   # gain, offset
    m.write_multiple(2, 110, list(words))                          # slave 2 is NODE-SY1
    PY

## Stage 6: front end attached

| Step | Procedure | Expected | If not |
| --- | --- | --- | --- |
| 6.1 | With the front end powered and no signal, read `mid`. | Both values of each pair within 8192 +/- 100 (1.65 V +/- 20 mV). | A value near 0 or 16383: V_MID is missing or a channel is saturated. |
| 6.2 | Proceed to the calibration and verification steps of the testing plan (Rev. 07, Case 3). | n/a | n/a |

## Items to report back

The results of 1.2 (crystal), 2.1 and 2.2 (rate and interrupt length), 3.4 and
3.5 (pair order and simultaneity), 4.2 (driver-enable timing), and 5.1 (stall
on a flash write) decide whether any of the assumptions listed in `README.md`
has to change.
