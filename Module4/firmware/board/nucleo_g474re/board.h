/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-20
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers
 * File:      Measurement Node, NUCLEO-G474RE Board Layer.
 *
 * Purpose:   Everything that ties node software to this particular board: pins used, node the
 *            image is built for, clock and sampling settings, and scaling from converter counts
 *            to volts and amperes.
 *
 * Usage:     node is selected @ build: -DNODE_SRC, or -DNODE_SY=1..4.
 *
 * Notes:     Pair: voltage input and current input of one MP.
 *            Output sample: one value per channel, after oversampling.
 *            Mid-rail: DC level on which AC input sits @ converter.
 *              pair 1  V PA0 (ADC1_IN1)    I PA1 (ADC2_IN2)      every node
 *              pair 2  V PA4 (ADC2_IN17)   I PB0 (ADC1_IN15)     every node
 *              pair 3  V PC0 (ADC1_IN6)    I PC1 (ADC2_IN7)      SRC only
 *              pair 4  V PC2 (ADC1_IN8)    I PC3 (ADC2_IN9)      SRC only
 *              TIM6    trigger of ADC1 + ADC2
 *              USART3  PB10 TX, PB11 RX, PB5 driver enable       RS-485 seg 1
 *              LPUART1 PA2 TX                                    console
 *              PA5 LD2 heartbeat; PC8 sample-rate test pin
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "stm32g4xx.h"
#include "meter.h"
#include "modbus_slave.h"
#include "node_app.h"
#include "regmap_node.h"

/* ---------------------------------------------------------------- identity */
#if defined(NODE_SRC)
#define NODE_NAME     "NODE-SRC"
#define NODE_ADDRESS  REG_ADDR_SRC   /* slave address */
#define NODE_PAIRS    4u             /* pairs on this node */
#elif defined(NODE_SY) && (NODE_SY >= 1) && (NODE_SY <= 4)
#define NODE_ADDRESS  ((uint8_t)(REG_ADDR_SY1 + (NODE_SY) - 1))   /* slave address */
#define NODE_PAIRS    2u                                          /* pairs on this node */
#if NODE_SY == 1
#define NODE_NAME     "NODE-SY1"
#elif NODE_SY == 2
#define NODE_NAME     "NODE-SY2"
#elif NODE_SY == 3
#define NODE_NAME     "NODE-SY3"
#else
#define NODE_NAME     "NODE-SY4"
#endif
#else
#error "Select the node: -DNODE_SRC, or -DNODE_SY=1 to 4"
#endif

/* ------------------------------------------------------------------ clocks */
#define SYSCLK_HZ      170000000u   /* system clock (Hz) */
#define ADC_TRIG_DIV   2767u        /* TIM6 reload value */
#define ADC_OSR        16u          /* conversions summed per output sample */
#define FS_HZ          ((float)SYSCLK_HZ / (float)ADC_TRIG_DIV / (float)ADC_OSR)   /* output sample rate (Hz) */

/* ------------------------------------------------- front end, nominal values */
#define ADC_VREF       3.3f                                   /* converter reference, nominal (V) */
#define ADC_LSB14      (ADC_VREF / 16384.0f)                  /* V per count @ converter pin */
#define DIV_K12        (10.0f / 160.0f)                       /* divider ratio, 12 V tier */
#define DIV_K24        (10.0f / 310.0f)                       /* divider ratio, 24 V tier */
#define INA_GAIN       20.0f                                  /* current amplifier gain */
#define V_PER_COUNT(k)     (ADC_LSB14 / (k))                  /* line V per count, for divider ratio k */
#define I_PER_COUNT(rsh)   (ADC_LSB14 / (INA_GAIN * (rsh)))   /* line A per count, for shunt rsh (ohm) */

/* -------------------------------------------------------- interrupt priority */
/* irq priorities; 0 = most urgent. */
#define PRIO_RS485     0u
#define PRIO_SAMPLING  1u
#define PRIO_TICK      2u
#define PRIO_DEBUG     3u

/* ----------------------------------------------------------------- modules */
/* diagnostic counters of board. */
typedef struct {
    uint32_t blocks;      /* output samples processed */
    uint32_t overruns;    /* sample blocks lost */
    uint32_t rx_errors;   /* RS-485 frames dropped */
    bool     hse;         /* true = crystal is clock source */
} board_diag_t;

extern node_app_t   g_node;   /* node application */
extern board_diag_t g_diag;   /* diagnostic counters */

bool clock_init(void);   /* set system clock; return true when crystal is in use */

void sampling_kinds(meter_kind_t kinds[REG_MAX_POINTS]);   /* fill in kind (AC or DC) of each point */
void sampling_init(void);                                  /* prepare sampling engine */
void sampling_start(void);                                 /* start sampling */
float sampling_mid(unsigned pair, bool current);           /* return tracked mid-rail level of channel (counts) */

void rs485_init(void);                                  /* prepare RS-485 driver */
bool rs485_frame(const uint8_t **frame, size_t *len);   /* return true, and frame, when complete frame is waiting */
void rs485_release(void);                               /* drop frame and resume listening */
void rs485_send(const uint8_t *data, size_t len);       /* send reply, then resume listening */

void dbg_init(void);                          /* prepare console */
void dbg_puts(const char *s);                 /* print string */
void dbg_u32(uint32_t v);                     /* print unsigned integer */
void dbg_fixed(float v, unsigned decimals);   /* print number w/ given decimals */

/* non-volatile store */
#define NV_CAL_LOADED     0x01u   /* flag: calibration was restored */
#define NV_ENERGY_LOADED  0x02u   /* flag: energy totals were restored */
#define NV_AVAILABLE      0x04u   /* flag: store is usable */
#ifndef NV_ENERGY_PERIOD_MS
#define NV_ENERGY_PERIOD_MS  60000u   /* interval between energy saves (ms) (CAP-M4-13) */
#endif
unsigned nv_init(node_app_t *a);                    /* restore stored state; return NV_* flags */
void     nv_task(node_app_t *a, uint32_t now_ms);   /* save state when due; called from main loop */

uint32_t board_millis(void);   /* return time since start-up (ms) */

#endif   /* BOARD_H */
