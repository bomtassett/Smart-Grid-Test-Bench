/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-14
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers
 * File:      Per-point Measurement Math (Design Report F.M4.2.7, eq. 18-21).
 *
 * Purpose:   Implementation of measurement math described in meter.h.
 */
#include "meter.h"
#include <math.h>
#include <string.h>

#define TWO_PI 6.28318530717958647692f
#define RAD_TO_DEG 57.2957795130823208768f

/* fill cfg with default settings. */
void meter_default_cfg(meter_cfg_t *c)
{
    c->fs = 3840.0f;
    c->f_nominal = 60.0f;
    c->f_min = 45.0f;
    c->v_floor = 0.10f;     /* CAP-M4-21 */
    c->i_floor = 0.0025f;   /* CAP-M4-21 */
    c->zc_hyst_frac = 0.10f;
    c->zc_hyst_min = 0.05f;
}

/* set rotation per sample of DFT reference for cycle of given length. */
static void set_rotation(meter_t *m, float cycle_len_samples)
{
    float w = TWO_PI / cycle_len_samples;
    m->rot_c = cosf(w);
    m->rot_s = sinf(w);
}

/* clear sums of cycle in progress. */
static void cycle_clear(meter_t *m)
{
    m->c_vv = m->c_ii = m->c_vi = m->c_v = m->c_i = m->c_len = 0.0f;
    m->c_vre = m->c_vim = m->c_ire = m->c_iim = 0.0f;
    m->ref_c = 1.0f;
    m->ref_s = 0.0f;
    m->n_since = 0;
    m->peak = 0.0f;
}

/* prepare meter for use. */
void meter_init(meter_t *m, meter_kind_t kind, const meter_cfg_t *cfg)
{
    memset(m, 0, sizeof *m);
    m->kind = kind;
    if (cfg) {
        m->cfg = *cfg;
    } else {
        meter_default_cfg(&m->cfg);
    }
    set_rotation(m, m->cfg.fs / m->cfg.f_nominal);
    cycle_clear(m);
}

/* add one sample, weighted by w, to sums of cycle in progress. */
static void accumulate(meter_t *m, float v, float i, float w)
{
    m->c_vv += w * v * v;
    m->c_ii += w * i * i;
    m->c_vi += w * v * i;
    m->c_v  += w * v;
    m->c_i  += w * i;
    m->c_len += w;
    /* correlation w/ DFT reference */
    m->c_vre += w * v * m->ref_c;
    m->c_vim -= w * v * m->ref_s;
    m->c_ire += w * i * m->ref_c;
    m->c_iim -= w * i * m->ref_s;
}

/* rotate DFT reference by one sample. */
static void advance_ref(meter_t *m)
{
    float c = m->ref_c * m->rot_c - m->ref_s * m->rot_s;
    m->ref_s = m->ref_s * m->rot_c + m->ref_c * m->rot_s;
    m->ref_c = c;
}

/* compute results of completed window and add to energy total. */
static void publish(meter_t *m)
{
    const meter_cfg_t *c = &m->cfg;
    meter_result_t *o = &m->out;
    double len = m->w_len;
    float vrms, irms, p;

    if (m->kind == METER_DC) {
        vrms = (float)(m->w_v / len);
        irms = (float)(m->w_i / len);
        if (fabsf(vrms) < c->v_floor) {
            vrms = 0.0f;
        }
        if (fabsf(irms) < c->i_floor) {
            irms = 0.0f;
        }
    } else {
        vrms = sqrtf((float)(m->w_vv / len));
        irms = sqrtf((float)(m->w_ii / len));
        if (vrms < c->v_floor) {
            vrms = 0.0f;
        }
        if (irms < c->i_floor) {
            irms = 0.0f;
        }
    }
    p = (float)(m->w_vi / len);

    o->window_s = (float)(len / c->fs);
    o->tracked = (m->kind == METER_AC) && (m->w_tracked == m->w_cycles) && vrms > 0.0f;
    o->vrms = vrms;
    o->irms = irms;
    o->freq = o->tracked ? (float)(c->fs * (double)m->w_cycles / len) : 0.0f;

    if (vrms == 0.0f || irms == 0.0f) {
        o->p = o->q = o->s = o->pf = o->ang_deg = 0.0f;
    } else if (m->kind == METER_DC) {
        o->p = p;
        o->q = o->s = o->pf = o->ang_deg = 0.0f;
    } else {
        float s = vrms * irms;
        float ang = 0.0f;
        if (o->tracked) {
            /* phase angle, V minus I */
            float re = (float)(m->w_vre * m->w_ire + m->w_vim * m->w_iim);
            float im = (float)(m->w_vim * m->w_ire - m->w_vre * m->w_iim);
            ang = atan2f(im, re) * RAD_TO_DEG;
        }
        float q2 = s * s - p * p;
        float q = (q2 > 0.0f) ? sqrtf(q2) : 0.0f;
        float pf = p / s;
        if (pf > 1.0f) {
            pf = 1.0f;
        } else if (pf < -1.0f) {
            pf = -1.0f;
        }
        o->p = p;
        o->s = s;
        o->q = (ang < 0.0f) ? -q : q;
        o->pf = pf;
        o->ang_deg = ang;
    }

    if (o->p > 0.0f) {
        /* energy total (eq. 21) */
        m->energy_mwh += (double)o->p * (len / c->fs) * (1000.0 / 3600.0);
    }

    m->w_vv = m->w_ii = m->w_vi = m->w_v = m->w_i = m->w_len = 0.0;
    m->w_vre = m->w_vim = m->w_ire = m->w_iim = 0.0;
    m->w_cycles = m->w_tracked = 0;
}

/* compute results of completed cycle. */
static void cycle_result(meter_t *m, bool tracked)
{
    const meter_cfg_t *c = &m->cfg;
    meter_cycle_t *y = &m->cyc;
    float len = m->c_len;
    float vrms = 0.0f, irms = 0.0f, p = 0.0f, q = 0.0f;

    if (len > 0.0f) {
        if (m->kind == METER_DC) {
            vrms = m->c_v / len;
            irms = m->c_i / len;
            if (fabsf(vrms) < c->v_floor) {
                vrms = 0.0f;
            }
            if (fabsf(irms) < c->i_floor) {
                irms = 0.0f;
            }
        } else {
            vrms = sqrtf(m->c_vv / len);
            irms = sqrtf(m->c_ii / len);
            if (vrms < c->v_floor) {
                vrms = 0.0f;
            }
            if (irms < c->i_floor) {
                irms = 0.0f;
            }
        }
        if (vrms != 0.0f && irms != 0.0f) {
            p = m->c_vi / len;
            if (m->kind == METER_AC) {
                float q2 = vrms * irms * vrms * irms - p * p;
                q = (q2 > 0.0f) ? sqrtf(q2) : 0.0f;
                if (tracked && (m->c_vim * m->c_ire - m->c_vre * m->c_iim) < 0.0f) {
                    q = -q;
                }
            }
        }
    }
    y->vrms = vrms;
    y->irms = irms;
    y->p = p;
    y->q = q;
    y->tracked = tracked && m->kind == METER_AC && vrms > 0.0f;
    y->freq = (y->tracked && len > 0.0f) ? c->fs / len : 0.0f;
    m->cycles++;
}

/* add completed cycle to window; return true when window has been completed. */
static bool close_cycle(meter_t *m, bool tracked)
{
    cycle_result(m, tracked);
    m->w_vv += m->c_vv;
    m->w_ii += m->c_ii;
    m->w_vi += m->c_vi;
    m->w_v  += m->c_v;
    m->w_i  += m->c_i;
    m->w_len += m->c_len;
    m->w_vre += m->c_vre;
    m->w_vim += m->c_vim;
    m->w_ire += m->c_ire;
    m->w_iim += m->c_iim;
    m->w_cycles++;
    if (tracked) {
        m->w_tracked++;
        set_rotation(m, m->c_len);   /* DFT reference set for next cycle */
    }
    m->peak_last = m->peak;
    cycle_clear(m);
    if (m->w_cycles >= METER_CYCLES_PER_WINDOW) {
        publish(m);
        return true;
    }
    return false;
}

/* take in one voltage and current sample; return true when window has been completed. */
bool meter_sample(meter_t *m, float v, float i)
{
    const meter_cfg_t *c = &m->cfg;
    bool closed = false;

    if (m->kind == METER_DC) {
        accumulate(m, v, i, 1.0f);
        m->n_since++;
        if ((float)m->n_since >= c->fs / c->f_nominal) {
            closed = close_cycle(m, false);
        }
        return closed;
    }

    /* rising zero crossing, w/ hysteresis */
    float hyst = c->zc_hyst_frac * m->peak_last;
    if (hyst < c->zc_hyst_min) {
        hyst = c->zc_hyst_min;
    }
    if (v < -hyst) {
        m->armed = true;
    }
    bool crossing = m->armed && m->v_prev < 0.0f && v >= 0.0f;

    if (crossing) {
        /* fraction of sample interval before crossing */
        float frac = -m->v_prev / (v - m->v_prev);
        m->armed = false;
        if (m->in_cycle) {
            accumulate(m, v, i, frac);
            closed = close_cycle(m, true);
        } else {
            cycle_clear(m);   /* partial cycle before first crossing is discarded */
            m->in_cycle = true;
        }
        /* remainder of interval starts next cycle */
        accumulate(m, v, i, 1.0f - frac);
        advance_ref(m);
        m->n_since = 1;
    } else {
        accumulate(m, v, i, 1.0f);
        advance_ref(m);
        m->n_since++;
        /* no crossing found: cycle is ended @ nominal length */
        if ((float)m->n_since >= (m->in_cycle ? c->fs / c->f_min : c->fs / c->f_nominal)) {
            m->in_cycle = false;
            closed = close_cycle(m, false);
        }
    }

    float a = fabsf(v);
    if (a > m->peak) {
        m->peak = a;
    }
    m->v_prev = v;
    return closed;
}

/* return energy total (mWh). */
uint32_t meter_energy_mwh(const meter_t *m)
{
    if (m->energy_mwh >= 4294967295.0) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)m->energy_mwh;
}

/* set energy total (mWh). */
void meter_set_energy_mwh(meter_t *m, uint32_t mwh)
{
    m->energy_mwh = (double)mwh;
}
