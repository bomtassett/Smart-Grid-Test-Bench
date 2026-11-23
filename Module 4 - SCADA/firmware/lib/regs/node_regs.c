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
 * Purpose:   Implementation of node register bank described in node_regs.h.
 *
 * Notes:     Every address is taken from generated/regmap_node.h.
 */
#include "node_regs.h"
#include <math.h>
#include <string.h>

/* return bit pattern of float. */
static uint32_t f32_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* return float w/ given bit pattern. */
static float bits_f32(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* prepare register bank for use. */
void node_regs_init(node_regs_t *r, uint8_t npoints, uint16_t fwver)
{
    memset(r, 0, sizeof *r);
    r->npoints = (npoints > REG_MAX_POINTS) ? (uint8_t)REG_MAX_POINTS : npoints;
    r->fwver = fwver;
    for (uint8_t ch = 0; ch < REG_CAL_CHANNELS; ch++) {
        node_regs_set_cal(r, ch, 1.0f, 0.0f);
    }
    r->cal_dirty = false;
}

/* return v, or 0 when v is not finite; *bad is set in that case. */
static float finite_or_zero(float v, bool *bad)
{
    if (!isfinite(v)) {
        *bad = true;
        return 0.0f;
    }
    return v;
}

/* store measured values of one point; value that is not finite is stored as 0 and FAULT is set. */
void node_regs_set_point(node_regs_t *r, uint8_t point, const node_meas_t *m)
{
    if (point >= r->npoints) {
        return;
    }
    bool bad = false;
    node_point_t p = r->live.pt[point];
    p.v[PT_FIELD(PT_OFF_VRMS)] = finite_or_zero(m->vrms, &bad);
    p.v[PT_FIELD(PT_OFF_IRMS)] = finite_or_zero(m->irms, &bad);
    p.v[PT_FIELD(PT_OFF_FREQ)] = finite_or_zero(m->freq, &bad);
    p.v[PT_FIELD(PT_OFF_ANG)]  = finite_or_zero(m->ang_deg, &bad);
    p.v[PT_FIELD(PT_OFF_PF)]   = finite_or_zero(m->pf, &bad);
    p.v[PT_FIELD(PT_OFF_P)]    = finite_or_zero(m->p, &bad);
    p.v[PT_FIELD(PT_OFF_Q)]    = finite_or_zero(m->q, &bad);
    p.v[PT_FIELD(PT_OFF_S)]    = finite_or_zero(m->s, &bad);
    NODE_REGS_LOCK();
    r->live.pt[point] = p;
    if (bad) {
        r->not_finite |= (uint8_t)(1u << point);
    } else {
        r->not_finite &= (uint8_t)~(1u << point);
    }
    if (r->not_finite) {
        r->live.status |= HLTH_STATUS_FAULT;
    } else {
        r->live.status &= (uint16_t)~HLTH_STATUS_FAULT;
    }
    NODE_REGS_UNLOCK();
}

/* store energy total of one point (mWh). */
void node_regs_set_energy_mwh(node_regs_t *r, uint8_t point, uint32_t mwh)
{
    if (point < r->npoints) {
        NODE_REGS_LOCK();
        r->live.pt[point].e = mwh;
        NODE_REGS_UNLOCK();
    }
}

/* return calibration gain of channel. */
float node_regs_cal_gain(const node_regs_t *r, uint8_t ch)
{
    return bits_f32(mb_u32_join(r->cal_w[ch][0], r->cal_w[ch][1]));
}

/* return calibration offset of channel. */
float node_regs_cal_offset(const node_regs_t *r, uint8_t ch)
{
    return bits_f32(mb_u32_join(r->cal_w[ch][2], r->cal_w[ch][3]));
}

/* set calibration gain and offset of channel. */
void node_regs_set_cal(node_regs_t *r, uint8_t ch, float gain, float offset)
{
    if (ch >= REG_CAL_CHANNELS) {
        return;
    }
    uint32_t g = f32_bits(gain), o = f32_bits(offset);
    r->cal_w[ch][0] = mb_u32_word(g, 0);
    r->cal_w[ch][1] = mb_u32_word(g, 1);
    r->cal_w[ch][2] = mb_u32_word(o, 0);
    r->cal_w[ch][3] = mb_u32_word(o, 1);
}

/* return node time last set by host (ms). */
uint32_t node_regs_time_set(const node_regs_t *r)
{
    return mb_u32_join(r->time_set_w[0], r->time_set_w[1]);
}

/* ----------------------------------------------------------- modbus callbacks */
/* copy live set to latched set. */
static void latch(void *ctx)
{
    node_regs_t *r = ctx;
    NODE_REGS_LOCK();
    r->latched = r->live;
    NODE_REGS_UNLOCK();
}

/* read one input reg. */
static mb_ex_t read_input(void *ctx, uint16_t a, uint16_t *v)
{
    const node_regs_t *r = ctx;
    const node_set_t *s = &r->latched;

    if (a < IR_POINT_BASE) {   /* header regs */
        switch (a) {
        case IR_HLTH_STATUS: *v = s->status; break;
        case IR_HLTH_SEQ:    *v = s->seq; break;
        default:             *v = mb_u32_word(s->time_ms, a - IR_HLTH_TIME); break;
        }
        return MB_EX_NONE;
    }
    if (a < IR_POINT_BASE + IR_POINT_BLOCK * r->npoints) {
        uint16_t off = (a - IR_POINT_BASE) % IR_POINT_BLOCK;
        const node_point_t *p = &s->pt[(a - IR_POINT_BASE) / IR_POINT_BLOCK];
        *v = mb_u32_word(f32_bits(p->v[PT_FIELD(off)]), off % 2u);
        return MB_EX_NONE;
    }
    if (a == IR_HLTH_FWVER) {   /* slow block */
        *v = r->fwver;
        return MB_EX_NONE;
    }
    if (a > IR_HLTH_FWVER && a < IR_ENERGY_BASE) {
        *v = 0;   /* reserved reg */
        return MB_EX_NONE;
    }
    if (a >= IR_ENERGY_BASE && a < IR_ENERGY_BASE + 2u * r->npoints) {
        *v = mb_u32_word(s->pt[(a - IR_ENERGY_BASE) / 2u].e, (a - IR_ENERGY_BASE) % 2u);
        return MB_EX_NONE;
    }
    return MB_EX_ILLEGAL_ADDRESS;
}

/* read one holding reg. */
static mb_ex_t read_holding(void *ctx, uint16_t a, uint16_t *v)
{
    const node_regs_t *r = ctx;
    if (a == HR_TIME_SET || a == HR_TIME_SET + 1u) {
        *v = r->time_set_w[a - HR_TIME_SET];
    } else if (a == HR_ENERGY_RESET) {
        *v = r->energy_reset_pending;   /* pending reset bits */
    } else if (a == HR_CTRL) {
        *v = r->ctrl;
    } else if (a >= HR_CAL_BASE && a < HR_CAL_BASE + 4u * REG_CAL_CHANNELS) {
        *v = r->cal_w[(a - HR_CAL_BASE) / 4u][(a - HR_CAL_BASE) % 4u];
    } else {
        return MB_EX_ILLEGAL_ADDRESS;
    }
    return MB_EX_NONE;
}

/* write one holding reg. */
static mb_ex_t write_holding(void *ctx, uint16_t a, uint16_t v)
{
    node_regs_t *r = ctx;
    if (a == HR_TIME_SET || a == HR_TIME_SET + 1u) {
        r->time_set_w[a - HR_TIME_SET] = v;
        r->time_set_pending = true;
    } else if (a == HR_ENERGY_RESET) {
        if (v >> r->npoints) {
            return MB_EX_ILLEGAL_VALUE;   /* bit of point not present */
        }
        r->energy_reset_pending |= v;
    } else if (a == HR_CTRL) {
        if (v & (uint16_t)~(CTRL_SAMPLE_EN | CTRL_REC_ARM)) {
            return MB_EX_ILLEGAL_VALUE;
        }
        r->ctrl = v;
        r->ctrl_dirty = true;
    } else if (a >= HR_CAL_BASE && a < HR_CAL_BASE + 4u * REG_CAL_CHANNELS) {
        r->cal_w[(a - HR_CAL_BASE) / 4u][(a - HR_CAL_BASE) % 4u] = v;
        r->cal_dirty = true;
    } else {
        return MB_EX_ILLEGAL_ADDRESS;
    }
    return MB_EX_NONE;
}

/* fill in slave description that serves this register bank. */
void node_regs_bind(node_regs_t *r, mb_slave_t *s, uint8_t address)
{
    s->address = address;
    s->read_input = read_input;
    s->read_holding = read_holding;
    s->write_holding = write_holding;
    s->latch = latch;
    s->commit = NULL;
    s->ctx = r;
}
