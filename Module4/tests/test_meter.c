/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-14
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (test)
 * File:      Tests: Measurement Math.
 *
 * Purpose:   Checks, on PC w/ no hardware, that measurement math gives correct values, by feeding
 *            it computed 3840 Hz waveforms whose true values are known.
 *
 * Notes:     Limits are Module 4 specifications; margin achieved is printed alongside.
 */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "meter.h"
#include "node_app.h"
#include "modbus_slave.h"

#define FS 3840.0   /* sample rate (Hz) */
#define PI 3.14159265358979323846

static int fails, checks;   /* failed checks, checks run */
/* count one check; print failure. */
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* test signal. */
typedef struct {
    double vrms, irms, f, phi_deg;   /* fundamental; phi > 0 = current lags */
    double h3, h5;                   /* harmonic fractions, both channels */
    double v_lsb, i_lsb;             /* quantization step @ input; 0 = none */
    double noise_lsb;                /* uniform noise, peak, LSB */
} sig_t;

/* return random number in -1..1. */
static double urand(void) { return (double)rand() / RAND_MAX * 2.0 - 1.0; }

/* return x w/ quantization and noise. */
static double quant(double x, double lsb, double noise_lsb)
{
    if (lsb <= 0.0) {
        return x;
    }
    return floor((x + noise_lsb * lsb * urand()) / lsb + 0.5) * lsb;
}

/* run meter for `seconds`; return last result.
 * phase runs continuously, so sample grid drifts against waveform. */
static meter_result_t run(meter_t *m, const sig_t *s, double seconds, double *t0, int *windows)
{
    long n = (long)(seconds * FS);
    for (long k = 0; k < n; k++) {
        double th = 2.0 * PI * s->f * (*t0 + (double)k / FS);
        double v = sqrt(2.0) * s->vrms * (sin(th) + s->h3 * sin(3 * th) + s->h5 * sin(5 * th));
        double thi = th - s->phi_deg * PI / 180.0;
        double i = sqrt(2.0) * s->irms * (sin(thi) + s->h3 * sin(3 * thi) + s->h5 * sin(5 * thi));
        if (meter_sample(m, (float)quant(v, s->v_lsb, s->noise_lsb), (float)quant(i, s->i_lsb, s->noise_lsb)) && windows) {
            (*windows)++;
        }
    }
    *t0 += (double)n / FS;
    return *meter_result(m);
}

/* return error of got against want (%). */
static double pct(double got, double want) { return 100.0 * fabs(got - want) / want; }

/* test resistive load. */
static void test_resistive(void)
{
    meter_t m;
    double t = 0.0123;
    int w = 0;
    meter_init(&m, METER_AC, NULL);
    sig_t s = {12.0, 0.545, 60.0, 0.0, 0, 0, 0, 0, 0};
    meter_result_t r = run(&m, &s, 2.0, &t, &w);
    printf("resistive 60 Hz: V %.4f  I %.5f  f %.4f  ang %.3f  P %.4f  PF %.4f  (%d windows in 2 s)\n",
           r.vrms, r.irms, r.freq, r.ang_deg, r.p, r.pf, w);
    CHECK(pct(r.vrms, 12.0) < 0.05 && pct(r.irms, 0.545) < 0.05);
    CHECK(fabs(r.freq - 60.0) < 0.002);
    CHECK(fabs(r.ang_deg) < 0.05 && r.pf > 0.9995);
    CHECK(pct(r.p, 12.0 * 0.545) < 0.05 && fabs(r.q) < 0.02);
    CHECK(w >= 19 && w <= 20);   /* 10 Hz publish (CAP-M4-19) */
    CHECK(fabs(r.window_s - 0.100) < 0.0005 && r.tracked);
}

/* test frequency accuracy (CAP-M4-03). */
static void test_frequency(void)
{
    /* CAP-M4-03: +/-0.02 Hz over 55..65 Hz, w/ converter quantization + noise */
    double worst_clean = 0, worst_noisy = 0, worst_v = 0;
    for (double f = 55.0; f <= 65.0001; f += 0.37) {
        for (int noisy = 0; noisy < 2; noisy++) {
            meter_t m;
            double t = 0.0031 * f;
            meter_init(&m, METER_AC, NULL);
            sig_t s = {12.0, 0.3, f, 0.0, 0, 0, noisy ? 0.0032 : 0, noisy ? 0.0002 : 0, noisy ? 2.0 : 0};
            run(&m, &s, 0.5, &t, NULL);
            for (int k = 0; k < 20; k++) {
                meter_result_t r = run(&m, &s, 0.1, &t, NULL);
                double e = fabs(r.freq - f);
                if (noisy && e > worst_noisy) worst_noisy = e;
                if (!noisy && e > worst_clean) worst_clean = e;
                if (pct(r.vrms, 12.0) > worst_v) worst_v = pct(r.vrms, 12.0);
            }
        }
    }
    printf("frequency 55 to 65 Hz: worst error %.5f Hz clean, %.5f Hz quantized with noise; worst V error %.3f%%\n",
           worst_clean, worst_noisy, worst_v);
    CHECK(worst_clean < 0.002);
    CHECK(worst_noisy < 0.02);
    CHECK(worst_v < 0.25);
}

/* test phase angle and sign of Q (CAP-M4-04). */
static void test_phase(void)
{
    /* CAP-M4-04: +/-1.0 deg. sign: positive angle and positive Q mean lagging current */
    double worst = 0;
    for (double phi = -80.0; phi <= 80.0; phi += 9.7) {
        meter_t m;
        double t = 0.004;
        meter_init(&m, METER_AC, NULL);
        sig_t s = {12.0, 0.5, 59.8, phi, 0, 0, 0.0032, 0.0002, 1.0};
        meter_result_t r = run(&m, &s, 1.0, &t, NULL);
        double e = fabs(r.ang_deg - phi);
        if (e > worst) worst = e;
        CHECK((phi > 1.0) == (r.q > 0.0) || fabs(phi) <= 1.0);
        CHECK(fabs(r.pf - cos(phi * PI / 180.0)) < 0.005);
    }
    printf("phase -80 to +80 deg at 59.8 Hz: worst error %.3f deg\n", worst);
    CHECK(worst < 0.2);

    /* Nucor case: PF 0.88 lagging */
    meter_t m;
    double t = 0;
    meter_init(&m, METER_AC, NULL);
    double phi = acos(0.88) * 180.0 / PI;
    sig_t s = {12.0, 0.57, 60.0, phi, 0, 0, 0, 0, 0};
    meter_result_t r = run(&m, &s, 1.0, &t, NULL);
    printf("PF 0.88 lagging: ang %.3f (want %.3f)  PF %.4f  P %.4f  Q %.4f  S %.4f\n", r.ang_deg, phi, r.pf, r.p, r.q, r.s);
    CHECK(fabs(r.pf - 0.88) < 0.002 && r.q > 0 && pct(r.s, 12.0 * 0.57) < 0.05);
    CHECK(pct(r.q, 12.0 * 0.57 * sin(phi * PI / 180.0)) < 0.3);
}

/* test RMS w/ harmonics (CAP-M4-22). */
static void test_harmonics(void)
{
    /* CAP-M4-22: sine + 20% third + 10% fifth vs clean sine of equal RMS */
    meter_t a, b;
    double ta = 0.002, tb = 0.002;
    double k = sqrt(1.0 + 0.04 + 0.01);
    meter_init(&a, METER_AC, NULL);
    meter_init(&b, METER_AC, NULL);
    sig_t dist = {12.0 / k, 0.5 / k, 60.0, 0.0, 0.20, 0.10, 0, 0, 0};
    sig_t clean = {12.0, 0.5, 60.0, 0.0, 0, 0, 0, 0, 0};
    meter_result_t rd = run(&a, &dist, 1.0, &ta, NULL);
    meter_result_t rc = run(&b, &clean, 1.0, &tb, NULL);
    printf("harmonics: distorted V %.4f against clean %.4f (%.3f%%); f %.4f\n", rd.vrms, rc.vrms, pct(rd.vrms, rc.vrms), rd.freq);
    CHECK(pct(rd.vrms, rc.vrms) < 1.0 && pct(rd.irms, rc.irms) < 1.0);
    CHECK(fabs(rd.freq - 60.0) < 0.02);
}

/* test light load current. */
static void test_light_current(void)
{
    /* 10% of load-channel full scale (0.1 A) through 14-bit converter: one count = 0.2 mA */
    meter_t m;
    double t = 0.001;
    meter_init(&m, METER_AC, NULL);
    sig_t s = {12.0, 0.100, 60.0, 0.0, 0, 0, 0.0032, 0.0002, 1.0};
    meter_result_t r = run(&m, &s, 1.0, &t, NULL);
    printf("light current 0.1 A quantized: I %.5f (%.3f%%)  P %.4f\n", r.irms, pct(r.irms, 0.1), r.p);
    CHECK(pct(r.irms, 0.1) < 0.5 && pct(r.p, 1.2) < 0.5);
}

/* test de-energized point (CAP-M4-21). */
static void test_deenergized(void)
{
    /* CAP-M4-21: de-energized point is reported as 0 and is still published @ 10 Hz */
    meter_t m;
    int w = 0;
    meter_init(&m, METER_AC, NULL);
    srand(7);
    for (int k = 0; k < (int)FS; k++) {
        if (meter_sample(&m, (float)(0.02 * urand()), (float)(0.0008 * urand()))) {
            w++;
        }
    }
    const meter_result_t *r = meter_result(&m);
    printf("de-energized: V %.4f  I %.5f  f %.2f  P %.4f  (%d windows in 1 s)\n", r->vrms, r->irms, r->freq, r->p, w);
    CHECK(r->vrms == 0 && r->irms == 0 && r->freq == 0 && r->p == 0 && r->s == 0);
    CHECK(w == 10 && meter_energy_mwh(&m) == 0);

    /* V present, load open: V and f are valid; I and P are 0 */
    double t = 0;
    sig_t s = {12.0, 0.0, 60.0, 0, 0, 0, 0.0032, 0.0002, 1.0};
    meter_result_t r2 = run(&m, &s, 1.0, &t, NULL);
    CHECK(pct(r2.vrms, 12.0) < 0.1 && r2.irms == 0 && r2.p == 0 && fabs(r2.freq - 60.0) < 0.02);
}

/* test frequency steps and loss of signal. */
static void test_step_and_loss(void)
{
    /* frequency steps as in Case 4, then loss of signal and its return */
    meter_t m;
    double t = 0;
    meter_init(&m, METER_AC, NULL);
    const double steps[] = {60.0, 57.5, 62.5, 56.0, 64.0, 55.0, 65.0};
    for (unsigned k = 0; k < sizeof steps / sizeof steps[0]; k++) {
        sig_t s = {7.07, 0.2, steps[k], 0, 0, 0, 0.0032, 0.0002, 1.0};
        meter_result_t r = run(&m, &s, 0.5, &t, NULL);
        CHECK(fabs(r.freq - steps[k]) < 0.02 && pct(r.vrms, 7.07) < 0.25);
    }
    sig_t off = {0.0, 0.0, 60.0, 0, 0, 0, 0, 0, 0};
    meter_result_t r = run(&m, &off, 0.5, &t, NULL);
    CHECK(r.vrms == 0 && r.freq == 0);
    sig_t on = {12.0, 0.3, 60.0, 0, 0, 0, 0, 0, 0};
    r = run(&m, &on, 0.5, &t, NULL);
    CHECK(pct(r.vrms, 12.0) < 0.05 && fabs(r.freq - 60.0) < 0.002);
}

/* test energy and DC point. */
static void test_energy_and_dc(void)
{
    /* Case 3 step 3: 12 V, 0.545 A, 10 min = ~1.09 Wh */
    meter_t m;
    double t = 0;
    meter_init(&m, METER_AC, NULL);
    sig_t s = {12.0, 0.545, 60.02, 0, 0, 0, 0, 0, 0};
    run(&m, &s, 600.0, &t, NULL);
    double want = 12.0 * 0.545 * 600.0 / 3.6;   /* mWh */
    printf("energy over 600 s: %u mWh (want %.1f)\n", (unsigned)meter_energy_mwh(&m), want);
    CHECK(pct(meter_energy_mwh(&m), want) < 0.1);
    meter_set_energy_mwh(&m, 0);
    CHECK(meter_energy_mwh(&m) == 0);

    meter_t d;
    int w = 0;
    meter_init(&d, METER_DC, NULL);
    for (int k = 0; k < (int)FS; k++) {
        if (meter_sample(&d, 12.03f, 1.25f)) {
            w++;
        }
    }
    const meter_result_t *r = meter_result(&d);
    CHECK(w == 10 && pct(r->vrms, 12.03) < 0.01 && pct(r->irms, 1.25) < 0.01 && pct(r->p, 12.03 * 1.25) < 0.01);
    CHECK(r->freq == 0 && r->q == 0);
}

/* test whole chain: samples -> node_app -> regs, read back over Modbus. */
static void test_node_app(void)
{
    node_app_t a;
    mb_slave_t s;
    uint8_t q[MB_MAX_ADU], p[MB_MAX_ADU];
    meter_kind_t kinds[2] = {METER_AC, METER_AC};
    node_app_init(&a, 2, kinds, NULL);
    node_regs_bind(&a.regs, &s, REG_ADDR_SY1);

    /* calibration: raw current of point 1 reads 2% low, so gain of 1.0204 is written by host */
    node_regs_set_cal(&a.regs, 3, 1.0204f, 0.0f);
    a.regs.cal_dirty = true;
    node_app_service(&a);

    for (long k = 0; k < (long)(1.5 * FS); k++) {
        double th = 2.0 * PI * 60.0 * (double)k / FS;
        node_app_sample(&a, PT_SY1_SUB, (float)(sqrt(2.0) * 24.0 * sin(th)), (float)(sqrt(2.0) * 0.2725 * sin(th)));
        node_app_sample(&a, PT_SY1_LOAD, (float)(sqrt(2.0) * 12.0 * sin(th)), (float)(sqrt(2.0) * 0.545 * 0.98 * sin(th)));
        if (k % 96 == 0) {   /* 3840 / 1000 is not whole; 1000 ms are ticked per 3840 samples */
            for (int j = 0; j < 25; j++) {
                node_app_tick_1ms(&a);
            }
        }
    }
    q[0] = REG_ADDR_SY1; q[1] = 0x04; q[2] = 0; q[3] = 0; q[4] = 0; q[5] = REG_SY1_FAST_READ;
    uint16_t crc = mb_crc16(q, 6);
    q[6] = (uint8_t)crc; q[7] = (uint8_t)(crc >> 8);
    size_t n = mb_slave_process(&s, q, 8, p, sizeof p, NULL);
    CHECK(n == 5u + 2u * REG_SY1_FAST_READ);
/* reg i of read response */
#define REG(i) ((uint16_t)((p[3 + 2 * (i)] << 8) | p[4 + 2 * (i)]))
    float fv[3];
    const unsigned at[3] = {IR_POINT(PT_SY1_SUB, PT_OFF_VRMS), IR_POINT(PT_SY1_LOAD, PT_OFF_IRMS), IR_POINT(PT_SY1_LOAD, PT_OFF_FREQ)};
    for (int j = 0; j < 3; j++) {   /* float32 in two regs, high word first */
        uint32_t u = mb_u32_join(REG(at[j]), REG(at[j] + 1));
        memcpy(&fv[j], &u, sizeof u);
    }
    printf("node_app over Modbus: SUB V %.4f V, LOAD I %.5f A, LOAD f %.4f Hz, SEQ %u, TIME %u ms\n",
           (double)fv[0], (double)fv[1], (double)fv[2], REG(IR_HLTH_SEQ),
           (unsigned)mb_u32_join(REG(IR_HLTH_TIME), REG(IR_HLTH_TIME + 1)));
    CHECK(fabsf(fv[0] - 24.0f) <= 0.012f);
    CHECK(fabsf(fv[1] - 0.545f) <= 0.0004f);   /* gain was applied */
    CHECK(fabsf(fv[2] - 60.0f) <= 0.002f);
    CHECK(fv[0] == a.regs.live.pt[PT_SY1_SUB].v[PT_FIELD(PT_OFF_VRMS)]);   /* reg holds meter value, unrounded */
    CHECK(REG(IR_HLTH_SEQ) == 1 && (REG(IR_HLTH_STATUS) & HLTH_STATUS_SAMPLING));
    CHECK(a.windows >= 28);

    /* energy is reset through pending flag */
    CHECK(meter_energy_mwh(&a.meter[PT_SY1_LOAD]) > 0);
    a.regs.energy_reset_pending = 1u << PT_SY1_LOAD;
    node_app_service(&a);
    CHECK(meter_energy_mwh(&a.meter[PT_SY1_LOAD]) == 0 && a.regs.live.pt[PT_SY1_LOAD].e == 0);
    CHECK(a.regs.live.pt[PT_SY1_SUB].e > 0);
}

/* run tests. */
int main(void)
{
    srand(1);
    test_resistive();
    test_frequency();
    test_phase();
    test_harmonics();
    test_light_current();
    test_deenergized();
    test_step_and_loss();
    test_energy_and_dc();
    test_node_app();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
