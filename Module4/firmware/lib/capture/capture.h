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
 * Purpose:   Keeps recording of what node measured just before and just after protection trip,
 *            so that fault can be examined afterwards.
 *
 * Notes:     Trip: opening of breaker on fault, signalled to node by TRIP edge.
 *            Record: data kept around one trip; it consists of rows and clip.
 *            Row: results of one cycle of one point. Clip: raw samples of one point.
 *            Ring: buffer that keeps only latest entries; used before trip.
 *            Armed: waiting for trip. Completed record is held until it is disarmed.
 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "meter.h"

#define CAP_MAX_POINTS   4u     /* most points */
#define CAP_PRE_ROWS     16u    /* rows kept per point before trip */
#define CAP_ROWS         160u   /* rows per point in record */
#define CAP_CLIP_PRE     192u   /* clip samples kept before trip */
#define CAP_CLIP_LEN     768u   /* clip samples in record */

/* state of fault record. */
typedef enum { CAP_IDLE = 0, CAP_ARMED = 1, CAP_RECORDING = 2, CAP_READY = 3 } cap_state_t;

/* one row. */
typedef struct {
    int32_t off;                      /* end of cycle, in samples from trip */
    float   vrms, irms, p, q, freq;   /* results of cycle */
} cap_row_t;

/* settings of fault record. */
typedef struct {
    float   fs;              /* sample rate (Hz) */
    float   pre_s, post_s;   /* time recorded before and after trip (s) */
    uint8_t npoints;         /* number of points */
    uint8_t clip_point;      /* point whose samples form clip */
    float   v_lsb, i_lsb;    /* clip scale: V per count, A per count */
} cap_cfg_t;

/* fault record of one node. */
typedef struct {
    cap_cfg_t cfg;             /* settings */
    uint32_t  pre_n, post_n;   /* samples recorded before and after trip */

    volatile uint8_t  state;         /* state */
    volatile uint8_t  arm_req;       /* pending arm or disarm request */
    volatile uint8_t  trip_req;      /* pending trip request */
    volatile uint32_t trip_req_ms;   /* node time of pending trip (ms) */

    uint32_t n;         /* sample count */
    uint32_t trip_n;    /* sample count @ trip */
    uint32_t trip_ms;   /* node time of trip (ms) */
    uint32_t records;   /* records completed */

    cap_row_t rows[CAP_MAX_POINTS][CAP_ROWS];   /* rows per point */
    uint16_t  row_head[CAP_MAX_POINTS];         /* next ring slot per point */
    uint16_t  row_count[CAP_MAX_POINTS];        /* rows held per point */
    uint16_t  row_pre[CAP_MAX_POINTS];          /* rows before trip per point */

    int16_t   clip_v[CAP_CLIP_LEN], clip_i[CAP_CLIP_LEN];   /* clip samples (V, I) */
    uint16_t  clip_head, clip_count, clip_pre;              /* next ring slot, samples held, samples before trip */
    bool      clip_clamped;                                 /* true = clip sample was clamped */
    bool      row_bad;                                      /* true = row value was not finite */
} capture_t;

/* fill cfg with default settings. */
void capture_default_cfg(cap_cfg_t *cfg, uint8_t npoints);

/* prepare fault record for use. */
void capture_init(capture_t *c, const cap_cfg_t *cfg);

/* arm record (on), or discard it and disarm (off); may be called from any context. */
void capture_arm(capture_t *c, bool on);

/* record TRIP edge and its node time; may be called from any context. */
void capture_trip(capture_t *c, uint32_t node_ms);

/* take in one sample of clip point. */
void capture_sample(capture_t *c, float v, float i);

/* take in results of completed cycle of one point. */
void capture_cycle(capture_t *c, uint8_t point, const meter_cycle_t *y);

/* return state of fault record. */
static inline cap_state_t capture_state(const capture_t *c) { return (cap_state_t)c->state; }

/* return rows of one point, oldest first; valid in CAP_READY. */
const cap_row_t *capture_rows(const capture_t *c, uint8_t point, uint16_t *count, uint16_t *pre);

/* return clip, oldest first; valid in CAP_READY. */
uint16_t capture_clip(const capture_t *c, const int16_t **v, const int16_t **i, uint16_t *pre);

/*
 * record as stream of 16-bit words. PROVISIONAL (D-28).
 * 32-bit values are placed high word first.
 *   header, 16 words   0 "RC" (5243h)  1 layout version (2)  2 points  3 clip point
 *                      4, 5 node time of trip, ms  6 sample rate, Hz
 *                      7 clip samples  8 clip samples before trip
 *                      9 clip V per count, uV  10 clip A per count, uA
 *                      11..14 rows of point 0..3  15 flags
 *   rows, 12 words     0, 1 offset from trip, samples (i32)
 *                      2, 3 V  4, 5 I  6, 7 P  8, 9 Q  10, 11 f (f32: V, A, W, var, Hz)
 *   clip               V counts (i16), then I counts (i16)
 */
#define CAP_HDR_WORDS  16u               /* header length (words) */
#define CAP_ROW_WORDS  12u               /* row length (words) */
#define CAP_MAGIC      0x5243u           /* first word of stream */
#define CAP_VERSION    2u                /* layout version */
#define CAP_FLAG_CLIP_CLAMPED  0x0001u   /* flag: clip sample was clamped */
#define CAP_FLAG_NOT_FINITE    0x0002u   /* flag: row value was not finite */

/* return length of stream (words); 0 when no record is ready. */
uint32_t capture_words(const capture_t *c);

/* copy words of stream to out; return number copied. */
uint16_t capture_read(const capture_t *c, uint32_t first, uint16_t count, uint16_t *out);

#endif   /* CAPTURE_H */
