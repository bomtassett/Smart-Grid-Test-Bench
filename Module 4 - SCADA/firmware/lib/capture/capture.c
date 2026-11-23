/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-21
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers
 * File:      Node Fault Record (CAP-M4-10, CAP-M4-36).
 *
 * Purpose:   Implementation of fault record described in capture.h.
 */
#include <math.h>
#include <string.h>
#include "capture.h"

/* fill cfg with default settings. */
void capture_default_cfg(cap_cfg_t *cfg, uint8_t npoints)
{
    cfg->fs = 3840.0f;
    cfg->pre_s = 0.2f;
    cfg->post_s = 2.0f;
    cfg->npoints = (npoints > CAP_MAX_POINTS) ? (uint8_t)CAP_MAX_POINTS : npoints;
    cfg->clip_point = 0;
    cfg->v_lsb = 0.002f;
    cfg->i_lsb = 0.0005f;
}

/* clear record and rings. */
static void clear(capture_t *c)
{
    memset(c->row_head, 0, sizeof c->row_head);
    memset(c->row_count, 0, sizeof c->row_count);
    memset(c->row_pre, 0, sizeof c->row_pre);
    c->clip_head = c->clip_count = c->clip_pre = 0;
    c->clip_clamped = false;
    c->row_bad = false;
}

/* prepare fault record for use. */
void capture_init(capture_t *c, const cap_cfg_t *cfg)
{
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    if (c->cfg.npoints > CAP_MAX_POINTS) {
        c->cfg.npoints = (uint8_t)CAP_MAX_POINTS;
    }
    if (c->cfg.clip_point >= c->cfg.npoints) {
        c->cfg.clip_point = 0;
    }
    c->pre_n = (uint32_t)lroundf(c->cfg.pre_s * c->cfg.fs);
    c->post_n = (uint32_t)lroundf(c->cfg.post_s * c->cfg.fs);
    c->state = CAP_IDLE;
}

/* arm record (on), or discard it and disarm (off); may be called from any context. */
void capture_arm(capture_t *c, bool on)
{
    c->arm_req = on ? 1u : 2u;
}

/* record TRIP edge and its node time; may be called from any context. */
void capture_trip(capture_t *c, uint32_t node_ms)
{
    if (c->state == CAP_ARMED && !c->trip_req) {
        c->trip_req_ms = node_ms;
        c->trip_req = 1u;
    }
}

/* convert value to clip counts; *clamped is set when it does not fit. */
static int16_t counts(float x, float lsb, bool *clamped)
{
    float k = x / lsb;
    if (k > 32767.0f) {
        *clamped = true;
        return 32767;
    }
    if (k < -32768.0f) {
        *clamped = true;
        return -32768;
    }
    return (int16_t)lroundf(k);
}

/* reverse a[lo..hi). */
static void rev_rows(cap_row_t *a, unsigned lo, unsigned hi)
{
    while (lo + 1u < hi) {
        cap_row_t t = a[lo];
        a[lo++] = a[--hi];
        a[hi] = t;
    }
}

/* reverse a[lo..hi). */
static void rev_i16(int16_t *a, unsigned lo, unsigned hi)
{
    while (lo + 1u < hi) {
        int16_t t = a[lo];
        a[lo++] = a[--hi];
        a[hi] = t;
    }
}

/* start record @ TRIP edge: rings are reordered oldest first. */
static void freeze(capture_t *c)
{
    c->trip_n = c->n - 1u;   /* sample count @ trip */
    c->trip_ms = c->trip_req_ms;

    for (unsigned p = 0; p < c->cfg.npoints; p++) {
        cap_row_t *r = c->rows[p];
        unsigned count = c->row_count[p];
        if (count == CAP_PRE_ROWS) {   /* ring full */
            unsigned h = c->row_head[p];
            rev_rows(r, 0, h);
            rev_rows(r, h, CAP_PRE_ROWS);
            rev_rows(r, 0, CAP_PRE_ROWS);
        }
        unsigned keep = 0;
        for (unsigned k = 0; k < count; k++) {   /* offsets made relative to trip */
            int32_t off = (int32_t)((uint32_t)r[k].off - c->trip_n);
            if (off > -(int32_t)c->pre_n) {
                r[keep] = r[k];
                r[keep].off = off;
                keep++;
            }
        }
        c->row_count[p] = c->row_pre[p] = (uint16_t)keep;
    }

    if (c->clip_count == CAP_CLIP_PRE) {
        unsigned h = c->clip_head;
        rev_i16(c->clip_v, 0, h); rev_i16(c->clip_v, h, CAP_CLIP_PRE); rev_i16(c->clip_v, 0, CAP_CLIP_PRE);
        rev_i16(c->clip_i, 0, h); rev_i16(c->clip_i, h, CAP_CLIP_PRE); rev_i16(c->clip_i, 0, CAP_CLIP_PRE);
    }
    c->clip_pre = c->clip_count;
    c->trip_req = 0;
    c->state = CAP_RECORDING;
}

/* take in one sample of clip point. */
void capture_sample(capture_t *c, float v, float i)
{
    uint8_t req = c->arm_req;
    if (req) {
        c->arm_req = 0;
        if (req == 2u) {
            c->state = CAP_IDLE;
            c->trip_req = 0;
        } else if (c->state == CAP_IDLE) {
            clear(c);
            c->state = CAP_ARMED;
        }
    }
    c->n++;

    if (c->state == CAP_ARMED) {
        if (c->trip_req) {
            freeze(c);   /* first sample after edge */
        } else {
            c->clip_v[c->clip_head] = counts(v, c->cfg.v_lsb, &c->clip_clamped);
            c->clip_i[c->clip_head] = counts(i, c->cfg.i_lsb, &c->clip_clamped);
            c->clip_head = (uint16_t)((c->clip_head + 1u) % CAP_CLIP_PRE);
            if (c->clip_count < CAP_CLIP_PRE) {
                c->clip_count++;
            }
            return;
        }
    }
    if (c->state == CAP_RECORDING) {
        if (c->clip_count < c->clip_pre + (CAP_CLIP_LEN - CAP_CLIP_PRE)) {
            c->clip_v[c->clip_count] = counts(v, c->cfg.v_lsb, &c->clip_clamped);
            c->clip_i[c->clip_count] = counts(i, c->cfg.i_lsb, &c->clip_clamped);
            c->clip_count++;
        }
        if (c->n - c->trip_n > c->post_n) {   /* window complete */
            c->records++;
            c->state = CAP_READY;
        }
    }
}

/* take in results of completed cycle of one point. */
void capture_cycle(capture_t *c, uint8_t point, const meter_cycle_t *y)
{
    if (point >= c->cfg.npoints) {
        return;
    }
    cap_row_t row = {0, y->vrms, y->irms, y->p, y->q, y->freq};
    if (c->state != CAP_ARMED && c->state != CAP_RECORDING) {
        return;
    }
    float *x[] = {&row.vrms, &row.irms, &row.p, &row.q, &row.freq};
    for (unsigned k = 0; k < sizeof x / sizeof x[0]; k++) {
        if (!isfinite(*x[k])) {
            *x[k] = 0.0f;
            c->row_bad = true;
        }
    }
    if (c->state == CAP_ARMED) {
        row.off = (int32_t)c->n;   /* made relative to trip by freeze() */
        c->rows[point][c->row_head[point]] = row;
        c->row_head[point] = (uint16_t)((c->row_head[point] + 1u) % CAP_PRE_ROWS);
        if (c->row_count[point] < CAP_PRE_ROWS) {
            c->row_count[point]++;
        }
    } else if (c->state == CAP_RECORDING && c->row_count[point] < CAP_ROWS) {
        row.off = (int32_t)(c->n - c->trip_n);
        c->rows[point][c->row_count[point]++] = row;
    }
}

/* return rows of one point, oldest first; valid in CAP_READY. */
const cap_row_t *capture_rows(const capture_t *c, uint8_t point, uint16_t *count, uint16_t *pre)
{
    bool ok = c->state == CAP_READY && point < c->cfg.npoints;
    *count = ok ? c->row_count[point] : 0u;
    if (pre) {
        *pre = ok ? c->row_pre[point] : 0u;
    }
    return ok ? c->rows[point] : NULL;
}

/* return clip, oldest first; valid in CAP_READY. */
uint16_t capture_clip(const capture_t *c, const int16_t **v, const int16_t **i, uint16_t *pre)
{
    if (c->state != CAP_READY) {
        return 0;
    }
    *v = c->clip_v;
    *i = c->clip_i;
    if (pre) {
        *pre = c->clip_pre;
    }
    return c->clip_count;
}

/* ---------------------------------------------------------------- word stream */
/* return length of stream (words); 0 when no record is ready. */
uint32_t capture_words(const capture_t *c)
{
    if (c->state != CAP_READY) {
        return 0;
    }
    uint32_t n = CAP_HDR_WORDS + 2u * c->clip_count;
    for (unsigned p = 0; p < c->cfg.npoints; p++) {
        n += CAP_ROW_WORDS * c->row_count[p];
    }
    return n;
}

/* return x as uint16, limited to its range. */
static uint16_t sat_u16(float x)
{
    return (x <= 0.0f) ? 0u : ((x >= 65535.0f) ? 65535u : (uint16_t)lroundf(x));
}

/* return high word (word 0) or low word (word 1) of float. */
static uint16_t f32_word(float f, uint32_t word)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return (uint16_t)(word == 0u ? (u >> 16) : (u & 0xFFFFu));
}

/* return word k of stream. */
static uint16_t word_at(const capture_t *c, uint32_t k)
{
    if (k < CAP_HDR_WORDS) {
        switch (k) {
        case 0:  return CAP_MAGIC;
        case 1:  return CAP_VERSION;
        case 2:  return c->cfg.npoints;
        case 3:  return c->cfg.clip_point;
        case 4:  return (uint16_t)(c->trip_ms >> 16);
        case 5:  return (uint16_t)c->trip_ms;
        case 6:  return sat_u16(c->cfg.fs);
        case 7:  return c->clip_count;
        case 8:  return c->clip_pre;
        case 9:  return sat_u16(c->cfg.v_lsb * 1.0e6f);
        case 10: return sat_u16(c->cfg.i_lsb * 1.0e6f);
        case 15: return (uint16_t)((c->clip_clamped ? CAP_FLAG_CLIP_CLAMPED : 0u) | (c->row_bad ? CAP_FLAG_NOT_FINITE : 0u));
        default: return (k - 11u < c->cfg.npoints) ? c->row_count[k - 11u] : 0u;
        }
    }
    k -= CAP_HDR_WORDS;
    for (unsigned p = 0; p < c->cfg.npoints; p++) {
        uint32_t span = CAP_ROW_WORDS * c->row_count[p];
        if (k < span) {
            const cap_row_t *r = &c->rows[p][k / CAP_ROW_WORDS];
            uint32_t w = k % CAP_ROW_WORDS;
            switch (w / 2u) {
            case 0:  return (uint16_t)(w == 0u ? ((uint32_t)r->off >> 16) : ((uint32_t)r->off & 0xFFFFu));
            case 1:  return f32_word(r->vrms, w % 2u);
            case 2:  return f32_word(r->irms, w % 2u);
            case 3:  return f32_word(r->p, w % 2u);
            case 4:  return f32_word(r->q, w % 2u);
            default: return f32_word(r->freq, w % 2u);
            }
        }
        k -= span;
    }
    if (k < c->clip_count) {
        return (uint16_t)c->clip_v[k];
    }
    k -= c->clip_count;
    return (k < c->clip_count) ? (uint16_t)c->clip_i[k] : 0u;
}

/* copy words of stream to out; return number copied. */
uint16_t capture_read(const capture_t *c, uint32_t first, uint16_t count, uint16_t *out)
{
    uint32_t total = capture_words(c);
    uint16_t n = 0;
    while (n < count && first + n < total) {
        out[n] = word_at(c, first + n);
        n++;
    }
    return n;
}
