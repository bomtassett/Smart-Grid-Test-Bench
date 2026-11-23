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
 * Purpose:   Implementation of node application described in node_app.h.
 */
#include "node_app.h"
#include <string.h>

/* copy calibration constants from register bank. */
static void load_cal(node_app_t *a)
{
    for (uint8_t ch = 0; ch < REG_CAL_CHANNELS; ch++) {
        a->gain[ch] = node_regs_cal_gain(&a->regs, ch);
        a->offset[ch] = node_regs_cal_offset(&a->regs, ch);
    }
}

/* prepare node application for use. */
void node_app_init(node_app_t *a, uint8_t npoints, const meter_kind_t *kinds, const meter_cfg_t *cfg)
{
    memset(a, 0, sizeof *a);
    node_regs_init(&a->regs, npoints, NODE_FWVER);
    for (uint8_t p = 0; p < a->regs.npoints; p++) {
        meter_init(&a->meter[p], kinds ? kinds[p] : METER_AC, cfg);
    }
    load_cal(a);
    a->sampling = true;
    /* fault record armed from start-up */
    a->regs.ctrl = CTRL_SAMPLE_EN | CTRL_REC_ARM;
    a->regs.live.status = HLTH_STATUS_SAMPLING | HLTH_STATUS_CAL_VALID;
    cap_cfg_t cc;
    capture_default_cfg(&cc, a->regs.npoints);
    cc.fs = cfg ? cfg->fs : cc.fs;
    capture_init(&a->cap, &cc);
    capture_arm(&a->cap, true);
}

/* take in one raw voltage and current sample of point, and apply calibration. */
void node_app_sample(node_app_t *a, uint8_t point, float v_raw, float i_raw)
{
    if (point >= a->regs.npoints || !a->sampling) {
        return;
    }
    float v = a->gain[2u * point] * v_raw + a->offset[2u * point];
    float i = a->gain[2u * point + 1u] * i_raw + a->offset[2u * point + 1u];
    meter_t *m = &a->meter[point];
    bool closed = meter_sample(m, v, i);
    if (point == a->cap.cfg.clip_point) {
        capture_sample(&a->cap, v, i);
    }
    if (m->cycles != a->cycles_seen[point]) {
        a->cycles_seen[point] = m->cycles;
        capture_cycle(&a->cap, point, meter_cycle(m));
    }
    if (closed) {
        const meter_result_t *o = meter_result(m);
        node_meas_t meas = {o->vrms, o->irms, o->freq, o->ang_deg, o->pf, o->p, o->q, o->s};
        node_regs_set_point(&a->regs, point, &meas);
        node_regs_set_energy_mwh(&a->regs, point, meter_energy_mwh(m));
        NODE_REGS_LOCK();
        a->regs.live.time_ms = a->time_ms;
        NODE_REGS_UNLOCK();
        a->windows++;
    }
}

/* advance node time by 1 ms. */
void node_app_tick_1ms(node_app_t *a)
{
    a->time_ms++;
    if (++a->ms_in_second >= 1000u) {
        a->ms_in_second = 0;
        NODE_REGS_LOCK();
        a->regs.live.seq++;
        NODE_REGS_UNLOCK();
    }
}

/* act on host's writes; called from main loop. */
void node_app_service(node_app_t *a)
{
    node_regs_t *r = &a->regs;
    if (r->time_set_pending) {
        r->time_set_pending = false;
        a->time_ms = node_regs_time_set(r);
    }
    if (r->energy_reset_pending) {
        for (uint8_t p = 0; p < r->npoints; p++) {
            if (r->energy_reset_pending & (1u << p)) {
                meter_set_energy_mwh(&a->meter[p], 0);
                node_regs_set_energy_mwh(r, p, 0);
            }
        }
        r->energy_reset_pending = 0;
    }
    if (r->cal_dirty) {
        r->cal_dirty = false;
        load_cal(a);
        /* board fw also stores constants in flash */
    }
    if (r->ctrl_dirty) {
        r->ctrl_dirty = false;
        capture_arm(&a->cap, (r->ctrl & CTRL_REC_ARM) != 0u);   /* held record is discarded when bit is cleared */
        a->sampling = (r->ctrl & CTRL_SAMPLE_EN) != 0u;
        NODE_REGS_LOCK();
        if (a->sampling) {
            r->live.status |= HLTH_STATUS_SAMPLING;
        } else {
            r->live.status &= (uint16_t)~HLTH_STATUS_SAMPLING;
        }
        NODE_REGS_UNLOCK();
    }
    bool ready = capture_state(&a->cap) == CAP_READY;
    if (ready != ((r->live.status & HLTH_STATUS_REC_READY) != 0u)) {
        NODE_REGS_LOCK();
        if (ready) {
            r->live.status |= HLTH_STATUS_REC_READY;
        } else {
            r->live.status &= (uint16_t)~HLTH_STATUS_REC_READY;
        }
        NODE_REGS_UNLOCK();
    }
}

/* record TRIP edge; may be called from irq. */
void node_app_trip(node_app_t *a)
{
    capture_trip(&a->cap, a->time_ms);
}
