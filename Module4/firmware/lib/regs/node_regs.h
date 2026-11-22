/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-14
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (interface to 1D)
 * File:      Node Register Bank (ICD-2).
 *
 * Purpose:   Holds values measurement node publishes (voltage, current, power, and so on) and
 *            presents them to host as Modbus registers.
 *
 * Notes:     Point: one MP, as published.
 *            Set: values of every point from one window, w/ status word, heartbeat, and node time.
 *            Live set: set being written by measurement task. Latched set: copy served to master.
 *            Channel: one voltage or current input; two per point.
 */
#ifndef NODE_REGS_H
#define NODE_REGS_H

#include <stdbool.h>
#include <stdint.h>
#include "modbus_slave.h"
#include "regmap_node.h"

/* lock held while live set is copied; defined by board fw. */
#ifndef NODE_REGS_LOCK
#define NODE_REGS_LOCK()
#define NODE_REGS_UNLOCK()
#endif

/* published values of one point. */
typedef struct {
    float    v[PT_FIELDS];   /* measured fields, indexed by PT_FIELD() */
    uint32_t e;              /* energy total (mWh) */
} node_point_t;

/* one set. */
typedef struct {
    uint16_t     status;               /* status word */
    uint16_t     seq;                  /* heartbeat */
    uint32_t     time_ms;              /* node time (ms) */
    node_point_t pt[REG_MAX_POINTS];   /* points */
} node_set_t;

/* register bank of one node. */
typedef struct {
    uint8_t    npoints;      /* number of points */
    uint16_t   fwver;        /* firmware version */
    uint8_t    not_finite;   /* bit i: point i holds value that was not finite */
    node_set_t live;         /* live set */
    node_set_t latched;      /* latched set */

    /* holding regs */
    uint16_t   time_set_w[2];                /* time set by host, as two words */
    uint16_t   ctrl;                         /* control word */
    uint16_t   cal_w[REG_CAL_CHANNELS][4];   /* calibration words per channel */

    /* requests from host writes */
    bool       time_set_pending;       /* true = time set was received */
    uint16_t   energy_reset_pending;   /* bit i: reset energy of point i */
    bool       cal_dirty;              /* true = calibration was written */
    bool       ctrl_dirty;             /* true = control word was written */
} node_regs_t;

/* measured values of one point. */
typedef struct {
    float vrms, irms, freq, ang_deg, pf, p, q, s;   /* RMS voltage, RMS current, frequency, phase angle, power factor, active, reactive, and apparent power */
} node_meas_t;

/* prepare register bank for use. */
void node_regs_init(node_regs_t *r, uint8_t npoints, uint16_t fwver);

/* store measured values of one point; value that is not finite is stored as 0 and FAULT is set. */
void node_regs_set_point(node_regs_t *r, uint8_t point, const node_meas_t *m);

/* store energy total of one point (mWh). */
void node_regs_set_energy_mwh(node_regs_t *r, uint8_t point, uint32_t mwh);

/* return calibration gain of channel. */
float node_regs_cal_gain(const node_regs_t *r, uint8_t channel);

/* return calibration offset of channel. */
float node_regs_cal_offset(const node_regs_t *r, uint8_t channel);

/* set calibration gain and offset of channel. */
void  node_regs_set_cal(node_regs_t *r, uint8_t channel, float gain, float offset);

/* return node time last set by host (ms). */
uint32_t node_regs_time_set(const node_regs_t *r);

/* fill in slave description that serves this register bank. */
void node_regs_bind(node_regs_t *r, mb_slave_t *s, uint8_t address);

#endif   /* NODE_REGS_H */
