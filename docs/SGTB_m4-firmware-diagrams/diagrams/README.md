# Module 4 firmware and software diagrams

Flowcharts, state charts, register layouts, and plain-language descriptions of the Module 4 node firmware, board firmware, and host software. Each flowchart follows a plain-language description of the same routine, with pseudo-code.

The figures describe m4fw step 12 (register map 0.2), as of November 22, 2026. Text is stored as outlines, so that every figure renders the same in any viewer.

## Conventions

| No. | Figure | Source |
| --- | --- | --- |
| 1 | [Module 4 firmware diagrams: conventions and index](01_module-4-firmware-diagrams-conventions-and-index.svg) | `m4fw step 12, directory revision 11, register map 0.2` |

## Node firmware

| No. | Figure | Source |
| --- | --- | --- |
| 2 | [Node firmware structure: plain-language description](02_node-firmware-structure-plain-language-description.svg) | `firmware/lib/node, meter, regs, modbus_rtu; sim/simnode.c` |
| 3 | [Node firmware structure](03_node-firmware-structure.svg) | `firmware/lib/node, meter, regs, modbus_rtu; sim/simnode.c` |
| 4 | [Sample path and time base: plain-language description](04_sample-path-and-time-base-plain-language-description.svg) | `firmware/lib/node/node_app.c` |
| 5 | [Sample path and time base](05_sample-path-and-time-base.svg) | `firmware/lib/node/node_app.c` |
| 6 | [Zero-crossing tracker: plain-language description](06_zero-crossing-tracker-plain-language-description.svg) | `firmware/lib/meter/meter.c: meter_sample()` |
| 7 | [Zero-crossing tracker](07_zero-crossing-tracker.svg) | `firmware/lib/meter/meter.c: meter_sample()` |
| 8 | [Cycle accumulators and cycle close: plain-language description](08_cycle-accumulators-and-cycle-close-plain-language-description.svg) | `firmware/lib/meter/meter.c: accumulate(), close_cycle()` |
| 9 | [Cycle accumulators and cycle close](09_cycle-accumulators-and-cycle-close.svg) | `firmware/lib/meter/meter.c: accumulate(), close_cycle()` |
| 10 | [Window results: plain-language description](10_window-results-plain-language-description.svg) | `firmware/lib/meter/meter.c: publish()` |
| 11 | [Window results](11_window-results.svg) | `firmware/lib/meter/meter.c: publish()` |
| 12 | [Frame reception and dispatch: plain-language description](12_frame-reception-and-dispatch-plain-language-description.svg) | `firmware/lib/modbus_rtu/modbus_slave.c: mb_slave_process()` |
| 13 | [Frame reception and dispatch](13_frame-reception-and-dispatch.svg) | `firmware/lib/modbus_rtu/modbus_slave.c: mb_slave_process()` |
| 14 | [Read request: plain-language description](14_read-request-plain-language-description.svg) | `firmware/lib/modbus_rtu/modbus_slave.c: mb_slave_process()` |
| 15 | [Read request, FC 03 and FC 04](15_read-request-fc-03-and-fc-04.svg) | `firmware/lib/modbus_rtu/modbus_slave.c: mb_slave_process()` |
| 16 | [Write request: plain-language description](16_write-request-plain-language-description.svg) | `firmware/lib/modbus_rtu/modbus_slave.c: mb_slave_process()` |
| 17 | [Write request, FC 06 and FC 16](17_write-request-fc-06-and-fc-16.svg) | `firmware/lib/modbus_rtu/modbus_slave.c: mb_slave_process()` |
| 18 | [Deferred actions: plain-language description](18_deferred-actions-plain-language-description.svg) | `firmware/lib/regs/node_regs.c; firmware/lib/node/node_app.c` |
| 19 | [Holding register writes and deferred actions](19_holding-register-writes-and-deferred-actions.svg) | `firmware/lib/regs/node_regs.c; firmware/lib/node/node_app.c` |

## Host: fieldbus and load control

| No. | Figure | Source |
| --- | --- | --- |
| 20 | [Host poll cycle: plain-language description](20_host-poll-cycle-plain-language-description.svg) | `host/poller.py: Poller.run()` |
| 21 | [Host poll cycle](21_host-poll-cycle.svg) | `host/poller.py: Poller.run()` |
| 22 | [Shed command: plain-language description](22_shed-command-plain-language-description.svg) | `host/poller.py: Poller._commands(), Poller._poll_lcu()` |
| 23 | [Shed command, acknowledgment, and confirmation](23_shed-command-acknowledgment-and-confirmation.svg) | `host/poller.py: Poller._commands(), Poller._poll_lcu()` |
| 24 | [Demand controller: plain-language description](24_demand-controller-plain-language-description.svg) | `host/demand.py: DemandController.step()` |
| 25 | [Demand controller](25_demand-controller.svg) | `host/demand.py: DemandController.step()` |

## Register map and Modbus RTU

| No. | Figure | Source |
| --- | --- | --- |
| 26 | [Node input registers](26_node-input-registers.svg) | `regmap/regmap.yaml; firmware/lib/regs/node_regs.c` |
| 27 | [Measurement point block](27_measurement-point-block.svg) | `regmap/regmap.yaml; generated/regmap_node.h` |
| 28 | [Status and control words](28_status-and-control-words.svg) | `regmap/regmap.yaml; firmware/lib/regs/node_regs.c` |
| 29 | [Node holding registers and calibration block](29_node-holding-registers-and-calibration-block.svg) | `regmap/regmap.yaml; firmware/lib/regs/node_regs.c` |
| 30 | [Load control unit registers](30_load-control-unit-registers.svg) | `regmap/regmap.yaml; regs/lcu_regs.h; host/config.yaml` |
| 31 | [Modbus RTU frames](31_modbus-rtu-frames.svg) | `firmware/lib/modbus_rtu/modbus_slave.c` |
| 32 | [Function codes, exception codes, and data types](32_function-codes-exception-codes-and-data-types.svg) | `firmware/lib/modbus_rtu/modbus_slave.h; regmap/regmap.yaml` |

## Board firmware for the NUCLEO-G474RE

| No. | Figure | Source |
| --- | --- | --- |
| 33 | [Board firmware for the NUCLEO-G474RE: index and call tree](33_board-firmware-for-the-nucleo-g474re-index-and-call-tree.svg) | `firmware/board/nucleo_g474re/ (m4fw step 5; fault record added in step 6)` |
| 34 | [Start-up and main loop: plain-language description](34_start-up-and-main-loop-plain-language-description.svg) | `firmware/board/nucleo_g474re/main.c: main()` |
| 35 | [Start-up and main loop](35_start-up-and-main-loop.svg) | `firmware/board/nucleo_g474re/main.c: main()` |
| 36 | [Interrupts and the register lock](36_interrupts-and-the-register-lock.svg) | `firmware/board/nucleo_g474re/main.c, board_lock.h, board.h` |
| 37 | [Clock set-up: plain-language description](37_clock-set-up-plain-language-description.svg) | `firmware/board/nucleo_g474re/clock.c: clock_init()` |
| 38 | [Clock set-up](38_clock-set-up.svg) | `firmware/board/nucleo_g474re/clock.c: clock_init()` |
| 39 | [Sampling engine set-up: plain-language description](39_sampling-engine-set-up-plain-language-description.svg) | `firmware/board/nucleo_g474re/sampling.c: sampling_init(), adc_setup(), sampling_start()` |
| 40 | [Sampling engine set-up](40_sampling-engine-set-up.svg) | `firmware/board/nucleo_g474re/sampling.c: sampling_init(), adc_setup(), sampling_start()` |
| 41 | [Converter pairs, scaling, and the DMA buffer](41_converter-pairs-scaling-and-the-dma-buffer.svg) | `firmware/board/nucleo_g474re/sampling.c, board.h` |
| 42 | [Sampling interrupt: plain-language description](42_sampling-interrupt-plain-language-description.svg) | `firmware/board/nucleo_g474re/sampling.c: DMA1_Channel1_IRQHandler(), process(), centre()` |
| 43 | [Sampling interrupt](43_sampling-interrupt.svg) | `firmware/board/nucleo_g474re/sampling.c: DMA1_Channel1_IRQHandler(), process(), centre()` |
| 44 | [RS-485 driver states: plain-language description](44_rs-485-driver-states-plain-language-description.svg) | `firmware/board/nucleo_g474re/rs485.c` |
| 45 | [RS-485 driver states](45_rs-485-driver-states.svg) | `firmware/board/nucleo_g474re/rs485.c` |
| 46 | [USART3 interrupt, reception: plain-language description](46_usart3-interrupt-reception-plain-language-description.svg) | `firmware/board/nucleo_g474re/rs485.c: USART3_IRQHandler()` |
| 47 | [USART3 interrupt: reception](47_usart3-interrupt-reception.svg) | `firmware/board/nucleo_g474re/rs485.c: USART3_IRQHandler()` |
| 48 | [USART3 interrupt, transmission: plain-language description](48_usart3-interrupt-transmission-plain-language-description.svg) | `firmware/board/nucleo_g474re/rs485.c: USART3_IRQHandler(), rs485_frame(), rs485_send(), rs485_release(), listen()` |
| 49 | [USART3 interrupt: transmission, and the driver calls](49_usart3-interrupt-transmission-and-the-driver-calls.svg) | `firmware/board/nucleo_g474re/rs485.c: USART3_IRQHandler(), rs485_frame(), rs485_send(), rs485_release(), listen()` |
| 50 | [Flash store layout](50_flash-store-layout.svg) | `firmware/board/nucleo_g474re/nv.c, stm32g474re.ld` |
| 51 | [Store start-up: plain-language description](51_store-start-up-plain-language-description.svg) | `firmware/board/nucleo_g474re/nv.c: nv_init()` |
| 52 | [Store start-up](52_store-start-up.svg) | `firmware/board/nucleo_g474re/nv.c: nv_init()` |
| 53 | [Store task: plain-language description](53_store-task-plain-language-description.svg) | `firmware/board/nucleo_g474re/nv.c: nv_task(), store()` |
| 54 | [Store task](54_store-task.svg) | `firmware/board/nucleo_g474re/nv.c: nv_task(), store()` |

## Fault record and controller link

| No. | Figure | Source |
| --- | --- | --- |
| 55 | [Fault record: plain-language description](55_fault-record-plain-language-description.svg) | `firmware/lib/capture/capture.c; firmware/lib/node/node_app.c` |
| 56 | [Fault record states](56_fault-record-states.svg) | `firmware/lib/capture/capture.c; firmware/lib/node/node_app.c` |
| 57 | [Fault record: sample and cycle intake](57_fault-record-sample-and-cycle-intake.svg) | `firmware/lib/capture/capture.c: capture_sample(), capture_cycle()` |
| 58 | [Fault record word stream](58_fault-record-word-stream.svg) | `firmware/lib/capture/capture.h: capture_words(), capture_read()` |
| 59 | [Controller link frames](59_controller-link-frames.svg) | `firmware/lib/icd4/icd4.h, icd4.c (ICD-4 rev 0.2, Tables ICD-4.3, ICD-4.4)` |
| 60 | [Controller link: plain-language description](60_controller-link-plain-language-description.svg) | `firmware/lib/icd4/icd4.c: icd4_link_rx(), icd4_link_tick(), icd4_link_take()` |
| 61 | [Controller link supervision](61_controller-link-supervision.svg) | `firmware/lib/icd4/icd4.c: icd4_link_rx(), icd4_link_tick(), icd4_link_take()` |

## Host: alarms, historian, and supervisory API

| No. | Figure | Source |
| --- | --- | --- |
| 62 | [Host alarms: plain-language description](62_host-alarms-plain-language-description.svg) | `host/poller.py: Poller._evaluate_alarms(), Poller._alarm()` |
| 63 | [Host alarms](63_host-alarms.svg) | `host/poller.py: Poller._evaluate_alarms(), Poller._alarm()` |
| 64 | [Historian: plain-language description](64_historian-plain-language-description.svg) | `host/poller.py: Poller._historize(); host/historian.py` |
| 65 | [Historian write path](65_historian-write-path.svg) | `host/poller.py: Poller._historize(); host/historian.py` |
| 66 | [Supervisory API: plain-language description](66_supervisory-api-plain-language-description.svg) | `host/api.py: need(), session_of(), login()` |
| 67 | [Supervisory API: session check and login](67_supervisory-api-session-check-and-login.svg) | `host/api.py: need(), session_of(), login()` |
| 68 | [Supervisory API: paths and roles](68_supervisory-api-paths-and-roles.svg) | `host/api.py: create_app() (ICD-3)` |

## Historian database

| No. | Figure | Source |
| --- | --- | --- |
| 69 | [Historian database as built](69_historian-database-as-built.svg) | `host/historian.py: SCHEMA` |
| 70 | [Historian database: tables not built, and open decisions](70_historian-database-tables-not-built-and-open-decisions.svg) | `host/historian.py; ER diagram v0.3 of October 17, 2026; Table ICD-5.2` |
