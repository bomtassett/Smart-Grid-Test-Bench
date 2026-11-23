/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-14
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers
 * File:      Node Application, Hardware-independent.
 *
 * Purpose:   Part of measurement node that is same on every board: samples are corrected by the
 *            calibration, measured, and published to Modbus registers, and host's writes are acted on.
 *
 * Notes:     Node time: node's clock (ms), set by host.
 *            Heartbeat: counter incremented once per second.
 *            TRIP: signal from switchyard controller that breaker has opened on fault.
 */
#ifndef NODE_APP_H
#define NODE_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "meter.h"
#include "capture.h"
#include "node_regs.h"

/* firmware version (major, minor); equal to register map version, which is checked by host. */
#define NODE_FWVER 0x0002u

/* state of node application. */
typedef struct {
    node_regs_t regs;                          /* register bank */
    meter_t     meter[REG_MAX_POINTS];         /* one meter per point */
    float       gain[REG_CAL_CHANNELS];        /* calibration gain per channel */
    float       offset[REG_CAL_CHANNELS];      /* calibration offset per channel */
    uint32_t    time_ms;                       /* node time (ms) */
    uint16_t    ms_in_second;                  /* ms within present second */
    bool        sampling;                      /* true = sampling enabled */
    uint32_t    windows;                       /* windows published */
    capture_t   cap;                           /* fault record */
    uint32_t    cycles_seen[REG_MAX_POINTS];   /* cycles already passed to fault record, per point */
} node_app_t;

/* prepare node application for use. */
void node_app_init(node_app_t *a, uint8_t npoints, const meter_kind_t *kinds, const meter_cfg_t *cfg);

/* take in one raw voltage and current sample of point, and apply calibration. */
void node_app_sample(node_app_t *a, uint8_t point, float v_raw, float i_raw);

/* advance node time by 1 ms. */
void node_app_tick_1ms(node_app_t *a);

/* act on host's writes; called from main loop. */
void node_app_service(node_app_t *a);

/* record TRIP edge; may be called from irq. */
void node_app_trip(node_app_t *a);

#endif   /* NODE_APP_H */
