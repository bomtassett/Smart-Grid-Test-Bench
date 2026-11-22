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
 * Purpose:   Turns stream of voltage and current samples of one measurement point into quantities
 *            operator reads: RMS voltage and current, active, reactive, and apparent power,
 *            power factor, phase angle, frequency, and energy.
 *
 * Notes:     MP: measurement point; one voltage input and one current input, sampled together.
 *            Cycle: one full period of V or I sinusoid; found from rising zero crossings of V.
 *            Window: group of consecutive cycles over which one set of results is computed.
 *            Meter: measurement state of one MP (meter_t).
 */
#ifndef METER_H
#define METER_H

#include <stdbool.h>
#include <stdint.h>

#define METER_CYCLES_PER_WINDOW 6u   /* cycles in one window */

/* kind of MP. */
typedef enum { METER_AC = 0, METER_DC = 1 } meter_kind_t;

/* settings of meter. */
typedef struct {
    float fs;             /* sample rate (Hz) */
    float f_nominal;      /* nominal frequency (Hz) */
    float f_min;          /* lowest frequency tracked (Hz) */
    float v_floor;        /* RMS voltage below which 0 is reported (V) */
    float i_floor;        /* RMS current below which 0 is reported (A) */
    float zc_hyst_frac;   /* zero-crossing hysteresis, as fraction of peak voltage */
    float zc_hyst_min;    /* minimum zero-crossing hysteresis (V) */
} meter_cfg_t;

/* results of one window. */
typedef struct {
    float vrms, irms;    /* RMS voltage (V), RMS current (A) */
    float freq;          /* frequency (Hz) */
    float ang_deg;       /* phase angle, V minus I (deg) */
    float p, q, s, pf;   /* active power (W), reactive power (var), apparent power (VA), power factor */
    float window_s;      /* window duration (s) */
    bool  tracked;       /* true = every cycle was found by zero crossings */
} meter_result_t;

/* results of one cycle. */
typedef struct {
    float vrms, irms;   /* RMS voltage (V), RMS current (A) */
    float p, q;         /* active power (W), reactive power (var) */
    float freq;         /* frequency (Hz) */
    bool  tracked;      /* true = cycle was found by zero crossings */
} meter_cycle_t;

/* measurement state of one MP. */
typedef struct {
    meter_cfg_t  cfg;    /* settings */
    meter_kind_t kind;   /* AC or DC */

    /* zero-crossing tracker */
    float  v_prev;            /* previous voltage sample */
    bool   armed;             /* true = ready to detect next crossing */
    bool   in_cycle;          /* true = cycle has started */
    float  peak, peak_last;   /* peak voltage of this cycle, of last cycle */
    uint32_t n_since;         /* samples since cycle started */

    /* DFT reference */
    float  ref_c, ref_s, rot_c, rot_s;   /* reference phasor (cos, sin), its rotation per sample (cos, sin) */

    /* sums over cycle in progress */
    float  c_vv, c_ii, c_vi, c_v, c_i, c_len;   /* V^2, I^2, V*I, V, I, sample count */
    float  c_vre, c_vim, c_ire, c_iim;          /* fundamental of V (re, im), of I (re, im) */

    /* sums over window in progress */
    double w_vv, w_ii, w_vi, w_v, w_i, w_len;   /* V^2, I^2, V*I, V, I, sample count */
    double w_vre, w_vim, w_ire, w_iim;          /* fundamental of V (re, im), of I (re, im) */
    uint32_t w_cycles, w_tracked;               /* cycles in window, cycles found by zero crossings */

    meter_result_t out;   /* results of last window */
    double energy_mwh;    /* energy total (mWh) */

    meter_cycle_t cyc;   /* results of last cycle */
    uint32_t cycles;     /* cycles completed */
} meter_t;

/* fill cfg with default settings. */
void meter_default_cfg(meter_cfg_t *cfg);

/* prepare meter for use. */
void meter_init(meter_t *m, meter_kind_t kind, const meter_cfg_t *cfg);

/* take in one voltage and current sample; return true when window has been completed. */
bool meter_sample(meter_t *m, float v, float i);

/* return results of last window. */
static inline const meter_result_t *meter_result(const meter_t *m) { return &m->out; }

/* return results of last cycle. */
static inline const meter_cycle_t *meter_cycle(const meter_t *m) { return &m->cyc; }

/* return energy total (mWh). */
uint32_t meter_energy_mwh(const meter_t *m);

/* set energy total (mWh). */
void     meter_set_energy_mwh(meter_t *m, uint32_t mwh);

#endif   /* METER_H */
