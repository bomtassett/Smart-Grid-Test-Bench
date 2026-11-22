/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-21
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (test)
 * File:      Tests: Node Fault Record (CAP-M4-10, CAP-M4-36).
 *
 * Purpose:   Checks, on PC w/ no hardware, that fault record holds right data around trip:
 *            first w/ capture library alone, then through node application w/ simulated fault.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "capture.h"
#include "node_app.h"

#define FS 3840.0   /* sample rate (Hz) */
#define PI 3.14159265358979323846

static int fails, checks;   /* failed checks, checks run */
/* count one check; print failure. */
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static capture_t cap;    /* fault record under test */
static node_app_t app;   /* node application under test */

static float wf(const uint16_t *w)   /* float32 from two words, high word first */
{
    uint32_t u = ((uint32_t)w[0] << 16) | w[1];
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* feed one 60 Hz "cycle" per 64 samples on each point; each cycle is identified by its values. */
static void feed(capture_t *c, long samples, long *k, int trip_at)
{
    for (long n = 0; n < samples; n++, (*k)++) {
        if (*k == trip_at) {
            capture_trip(c, 123456u);
        }
        capture_sample(c, (float)(*k % 1000) * 0.01f, (float)(*k % 100) * 0.001f);
        if (*k % 64 == 63) {
            for (uint8_t p = 0; p < c->cfg.npoints; p++) {
                meter_cycle_t y = {(float)(*k / 64), (float)p, 1.0f, -2.0f, 60.0f, true};
                capture_cycle(c, p, &y);
            }
        }
    }
}

/* test capture library alone. */
static void test_library(void)
{
    cap_cfg_t cfg;
    capture_default_cfg(&cfg, 2);
    capture_init(&cap, &cfg);
    long k = 0;
    uint16_t n, pre;

    CHECK(cap.pre_n == 768 && cap.post_n == 7680);
    feed(&cap, 2000, &k, -1);
    CHECK(capture_state(&cap) == CAP_IDLE && capture_words(&cap) == 0);   /* not armed: nothing is kept */
    capture_trip(&cap, 1u);
    feed(&cap, 10, &k, -1);
    CHECK(capture_state(&cap) == CAP_IDLE);   /* trip while idle is ignored */

    capture_arm(&cap, true);
    feed(&cap, 5000, &k, -1);   /* rings wrap several times */
    CHECK(capture_state(&cap) == CAP_ARMED);
    long trip = k + 37;   /* mid-cycle */
    feed(&cap, 37 + 7680, &k, (int)trip);
    CHECK(capture_state(&cap) == CAP_RECORDING);
    CHECK(capture_rows(&cap, 0, &n, &pre) == NULL && n == 0);   /* record cannot be read until it is complete */
    feed(&cap, 1, &k, -1);
    CHECK(capture_state(&cap) == CAP_READY && cap.records == 1 && cap.trip_ms == 123456u);

    for (uint8_t p = 0; p < 2; p++) {
        const cap_row_t *r = capture_rows(&cap, p, &n, &pre);
        CHECK(r != NULL && pre == 12 && n == 132);   /* 0.2 s before, 2.0 s after, @ 60 Hz */
        int ordered = 1, values = 1;
        for (uint16_t j = 0; j < n; j++) {
            ordered &= (j == 0 || r[j].off == r[j - 1].off + 64);
            /* cycle ending @ absolute sample e was numbered (e - 1) / 64 */
            long e = trip + r[j].off;
            values &= (r[j].vrms == (float)((e - 1) / 64) && r[j].irms == (float)p && r[j].q == -2.0f);
        }
        CHECK(ordered && values);
        CHECK(r[0].off > -768 && r[0].off <= -768 + 64);   /* oldest row lies inside 0.2 s */
        CHECK(r[pre - 1].off <= 0 && r[pre].off > 0);      /* trip lies between these two rows */
        CHECK(r[n - 1].off <= 7680 && r[n - 1].off > 7680 - 64);
    }

    const int16_t *v, *i;
    uint16_t cn = capture_clip(&cap, &v, &i, &pre);
    CHECK(cn == CAP_CLIP_LEN && pre == CAP_CLIP_PRE);
    int clip_ok = 1;
    for (uint16_t j = 0; j < cn; j++) {
        /* clip sample j was fed @ absolute index trip - pre + j,
         * except sample @ trip itself, which opens post-trip part */
        long a = trip - pre + j;
        clip_ok &= (v[j] == (int16_t)lroundf((float)(a % 1000) * 0.01f / 0.002f));
        clip_ok &= (i[j] == (int16_t)lroundf((float)(a % 100) * 0.001f / 0.0005f));
    }
    CHECK(clip_ok && !cap.clip_clamped);

    /* word stream */
    uint32_t total = capture_words(&cap);
    CHECK(total == 16u + 2u * 132u * 12u + 2u * 768u);
    static uint16_t w[8192];
    uint32_t got = 0;
    while (got < total) {   /* read in pages of 120 words */
        uint16_t m = capture_read(&cap, got, 120, &w[got]);
        CHECK(m > 0);
        got += m;
    }
    CHECK(got == total && capture_read(&cap, total, 10, w + total) == 0);
    CHECK(w[0] == CAP_MAGIC && w[1] == CAP_VERSION && w[2] == 2 && w[3] == 0);
    CHECK((((uint32_t)w[4] << 16) | w[5]) == 123456u && w[6] == 3840 && w[7] == 768 && w[8] == 192);
    CHECK(w[9] == 2000 && w[10] == 500 && w[11] == 132 && w[12] == 132 && w[13] == 0 && w[15] == 0);
    const cap_row_t *r0 = capture_rows(&cap, 0, &n, &pre);
    const uint16_t *row = &w[16];
    CHECK((int32_t)(((uint32_t)row[0] << 16) | row[1]) == r0[0].off);
    CHECK(wf(&row[2]) == r0[0].vrms && wf(&row[2]) > 65.535f);   /* value old mV word could not hold */
    CHECK(wf(&row[4]) == 0.0f && wf(&row[6]) == 1.0f && wf(&row[8]) == -2.0f && wf(&row[10]) == 60.0f);
    CHECK(row[6] == 0x3F80u && row[7] == 0x0000u);   /* 1.0 = 3F800000h, high word first */
    const uint16_t *row1 = &w[16 + 132 * 12];        /* first row of point 1 */
    CHECK(wf(&row1[4]) == 1.0f);
    CHECK((int16_t)w[16 + 2 * 132 * 12] == v[0] && (int16_t)w[16 + 2 * 132 * 12 + 768] == i[0]);

    /* record is held until disarmed; second trip changes nothing */
    capture_trip(&cap, 999u);
    feed(&cap, 9000, &k, -1);
    CHECK(capture_state(&cap) == CAP_READY && cap.trip_ms == 123456u && cap.records == 1);
    capture_arm(&cap, true);
    feed(&cap, 1, &k, -1);
    CHECK(capture_state(&cap) == CAP_READY);   /* held record is not discarded by arming */
    capture_arm(&cap, false);
    feed(&cap, 1, &k, -1);
    CHECK(capture_state(&cap) == CAP_IDLE && capture_words(&cap) == 0);

    /* trip soon after arming: record starts w/ what has been collected */
    capture_arm(&cap, true);
    feed(&cap, 100, &k, -1);
    feed(&cap, 7681, &k, (int)k);
    CHECK(capture_state(&cap) == CAP_READY && cap.records == 2);
    capture_rows(&cap, 0, &n, &pre);
    CHECK(pre <= 2 && n == pre + 120);
    CHECK(capture_clip(&cap, &v, &i, &pre) == 100 + 576 && pre == 100);

    /* raw sample outside clip range is clamped and flagged */
    capture_arm(&cap, false);
    feed(&cap, 1, &k, -1);
    capture_arm(&cap, true);
    feed(&cap, 10, &k, -1);
    capture_sample(&cap, 100.0f, -100.0f);
    capture_trip(&cap, 5u);
    for (int j = 0; j < 7681; j++) {
        capture_sample(&cap, 0.0f, 0.0f);
    }
    CHECK(capture_state(&cap) == CAP_READY && cap.clip_clamped);
    cn = capture_clip(&cap, &v, &i, &pre);
    CHECK(v[pre - 1] == 32767 && i[pre - 1] == -32768);
    capture_read(&cap, 15, 1, w);
    CHECK(w[0] == CAP_FLAG_CLIP_CLAMPED);

    /* row value that is not finite is stored as 0 and flagged */
    capture_arm(&cap, false);
    feed(&cap, 1, &k, -1);
    capture_arm(&cap, true);
    feed(&cap, 10, &k, -1);
    meter_cycle_t bad = {24.0f, NAN, 1.0f, INFINITY, 60.0f, true};
    capture_cycle(&cap, 0, &bad);
    capture_trip(&cap, 6u);
    for (int j = 0; j < 7681; j++) {
        capture_sample(&cap, 0.0f, 0.0f);
    }
    const cap_row_t *rb = capture_rows(&cap, 0, &n, &pre);
    CHECK(capture_state(&cap) == CAP_READY && n == 1 && rb[0].vrms == 24.0f && rb[0].irms == 0.0f && rb[0].q == 0.0f);
    capture_read(&cap, 15, 1, w);
    CHECK(w[0] == CAP_FLAG_NOT_FINITE);
}

/* through node: 24 V, 0.444 A @ 60.2 Hz on point 0.
 * @ trip, current rises to fault level and voltage sags. */
static void test_node(void)
{
    meter_cfg_t cfg;
    meter_default_cfg(&cfg);
    node_app_init(&app, 2, NULL, &cfg);
    CHECK((app.regs.ctrl & CTRL_REC_ARM) && !(app.regs.live.status & HLTH_STATUS_REC_READY));

    const double f = 60.2;
    long trip_k = (long)(1.5 * FS) + 11;
    for (long k = 0; k < (long)(4.0 * FS); k++) {
        if (k % 4 == 0) {   /* ~1 kHz */
            node_app_tick_1ms(&app);
        }
        bool fault = k >= trip_k - 96;   /* fault begins 25 ms before trip */
        double th = 2.0 * PI * f * (double)k / FS;
        double v = (fault ? 14.0 : 24.0) * sqrt(2.0) * sin(th);
        double i = (fault ? 1.05 : 0.444) * sqrt(2.0) * sin(th - 0.3);
        if (k == trip_k) {
            node_app_trip(&app);
        }
        node_app_sample(&app, 0, (float)v, (float)i);
        node_app_sample(&app, 1, (float)(v * 0.5), (float)(i * 0.5));
        if (k % 384 == 0) {
            node_app_service(&app);
        }
    }
    node_app_service(&app);
    CHECK(capture_state(&app.cap) == CAP_READY);
    CHECK(app.regs.live.status & HLTH_STATUS_REC_READY);
    CHECK(app.cap.trip_ms >= (uint32_t)(trip_k / 4) && app.cap.trip_ms <= (uint32_t)(trip_k / 4) + 1u);   /* within 1 ms */

    uint16_t n, pre;
    const cap_row_t *r = capture_rows(&app.cap, 0, &n, &pre);
    CHECK(pre >= 11 && pre <= 13 && n >= 131 && n <= 134);
    CHECK(fabsf(r[0].vrms - 24.0f) < 0.05f && fabsf(r[0].irms - 0.444f) < 0.002f && fabsf(r[0].freq - 60.2f) < 0.05f);
    CHECK(fabsf(r[0].p - 24.0f * 0.444f * (float)cos(0.3)) < 0.05f && r[0].q > 0.0f);
    CHECK(fabsf(r[n - 1].vrms - 14.0f) < 0.05f && fabsf(r[n - 1].irms - 1.05f) < 0.005f);
    int step = -1;   /* first row @ fault level */
    for (uint16_t j = 0; j < n && step < 0; j++) {
        if (r[j].irms > 1.0f) {
            step = j;
        }
    }
    CHECK(step >= 0 && r[step].off > -96 - 70 && r[step].off <= 64);   /* fault appears in rows where it began */
    printf("  node record: %u rows (%u before the trip), fault level first seen at offset %ld samples\n",
           (unsigned)n, (unsigned)pre, (long)r[step].off);

    const int16_t *v, *i;
    uint16_t cn = capture_clip(&app.cap, &v, &i, &pre);
    CHECK(cn == CAP_CLIP_LEN && pre == CAP_CLIP_PRE);
    int16_t vmax_pre = 0, imax_post = 0;
    for (uint16_t j = 0; j < 64; j++) {
        vmax_pre = (v[j] > vmax_pre) ? v[j] : vmax_pre;
    }
    for (uint16_t j = pre; j < cn; j++) {
        imax_post = (i[j] > imax_post) ? i[j] : imax_post;
    }
    CHECK(fabs(vmax_pre * 0.002 - 24.0 * sqrt(2.0)) < 0.2);      /* pre-fault peak, found in clip */
    CHECK(fabs(imax_post * 0.0005 - 1.05 * sqrt(2.0)) < 0.02);   /* fault current peak, found in clip */

    /* record is discarded by host by clearing REC_ARM, and armed again by setting it */
    app.regs.ctrl = CTRL_SAMPLE_EN;
    app.regs.ctrl_dirty = true;
    node_app_service(&app);
    node_app_sample(&app, 0, 0.0f, 0.0f);
    node_app_service(&app);
    CHECK(capture_state(&app.cap) == CAP_IDLE && !(app.regs.live.status & HLTH_STATUS_REC_READY));
    app.regs.ctrl = CTRL_SAMPLE_EN | CTRL_REC_ARM;
    app.regs.ctrl_dirty = true;
    node_app_service(&app);
    node_app_sample(&app, 0, 0.0f, 0.0f);
    CHECK(capture_state(&app.cap) == CAP_ARMED);
}

/* run tests. */
int main(void)
{
    test_library();
    test_node();
    printf("test_capture: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
