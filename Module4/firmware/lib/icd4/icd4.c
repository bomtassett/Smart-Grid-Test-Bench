/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-21
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (link between two local controllers)
 * File:      Controller Link Frames & Link Supervision (ICD-4 rev 0.2).
 *
 * Purpose:   Implementation of controller link described in icd4.h.
 *
 * Notes:     Byte order, CRC, and enum values are PROVISIONAL (see icd4.h).
 */
#include <string.h>
#include "icd4.h"
#include "modbus_slave.h"   /* mb_crc16 */

/* put16, put32, putf: store value @ p, low byte first; return next position.
 * get16, get32, getf: read value @ p, low byte first; return next position. */
static uint8_t *put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return p + 2; }
static uint8_t *put32(uint8_t *p, uint32_t v) { p = put16(p, (uint16_t)v); return put16(p, (uint16_t)(v >> 16)); }
static uint8_t *putf(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); return put32(p, u); }
static const uint8_t *get16(const uint8_t *p, uint16_t *v) { *v = (uint16_t)(p[0] | (p[1] << 8)); return p + 2; }
static const uint8_t *get32(const uint8_t *p, uint32_t *v)
{
    *v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return p + 4;
}
static const uint8_t *getf(const uint8_t *p, float *f) { uint32_t u; p = get32(p, &u); memcpy(f, &u, 4); return p; }

/* check length and CRC of frame. */
static icd4_err_t check(const uint8_t *buf, size_t len, size_t want)
{
    if (len != want) {
        return ICD4_ERR_LENGTH;
    }
    uint16_t crc = (uint16_t)(buf[want - 2u] | (buf[want - 1u] << 8));
    return (mb_crc16(buf, want - 2u) == crc) ? ICD4_OK : ICD4_ERR_CRC;
}

/* encode controller frame; return its length. */
size_t icd4_encode_ctrl(const icd4_ctrl_t *f, uint8_t out[ICD4_CTRL_LEN])
{
    uint8_t *p = out;
    p = put16(p, f->seq);
    p = put32(p, f->t_ms);
    p = put16(p, f->sw_state);
    *p++ = f->trip_evt;
    *p++ = f->fault_type;
    *p++ = f->flisr_state;
    for (int k = 0; k < 3; k++) {
        p = putf(p, f->i_prot[k]);
    }
    *p++ = f->reason_code;
    *p++ = f->donor_id;
    p = put32(p, f->restore_ms);
    *p++ = f->alarms;
    p = put16(p, mb_crc16(out, (size_t)(p - out)));
    return (size_t)(p - out);
}

/* decode controller frame. */
icd4_err_t icd4_decode_ctrl(const uint8_t *buf, size_t len, icd4_ctrl_t *f)
{
    icd4_err_t e = check(buf, len, ICD4_CTRL_LEN);
    if (e != ICD4_OK) {
        return e;
    }
    const uint8_t *p = buf;
    p = get16(p, &f->seq);
    p = get32(p, &f->t_ms);
    p = get16(p, &f->sw_state);
    f->trip_evt = *p++;
    f->fault_type = *p++;
    f->flisr_state = *p++;
    for (int k = 0; k < 3; k++) {
        p = getf(p, &f->i_prot[k]);
    }
    f->reason_code = *p++;
    f->donor_id = *p++;
    p = get32(p, &f->restore_ms);
    f->alarms = *p;
    return ICD4_OK;
}

/* encode node frame; return its length. */
size_t icd4_encode_node(const icd4_node_t *f, uint8_t out[ICD4_NODE_LEN])
{
    uint8_t *p = out;
    p = put16(p, f->seq);
    p = put32(p, f->t_ms);
    p = putf(p, f->v_sub);
    p = putf(p, f->i_sub);
    p = putf(p, f->p_sub);
    p = putf(p, f->p_load);
    p = put16(p, f->health);
    p = put16(p, mb_crc16(out, (size_t)(p - out)));
    return (size_t)(p - out);
}

/* decode node frame. */
icd4_err_t icd4_decode_node(const uint8_t *buf, size_t len, icd4_node_t *f)
{
    icd4_err_t e = check(buf, len, ICD4_NODE_LEN);
    if (e != ICD4_OK) {
        return e;
    }
    const uint8_t *p = buf;
    p = get16(p, &f->seq);
    p = get32(p, &f->t_ms);
    p = getf(p, &f->v_sub);
    p = getf(p, &f->i_sub);
    p = getf(p, &f->p_sub);
    p = getf(p, &f->p_load);
    get16(p, &f->health);
    return ICD4_OK;
}

/* return change flags between two controller frames. */
uint8_t icd4_changes(const icd4_ctrl_t *a, const icd4_ctrl_t *b)
{
    uint8_t c = 0;
    c |= (a->sw_state != b->sw_state) ? ICD4_CHG_SWITCH : 0u;
    c |= (a->trip_evt != b->trip_evt || a->fault_type != b->fault_type) ? ICD4_CHG_TRIP : 0u;
    c |= (a->flisr_state != b->flisr_state) ? ICD4_CHG_FLISR : 0u;
    c |= (a->reason_code != b->reason_code) ? ICD4_CHG_REASON : 0u;
    c |= (a->donor_id != b->donor_id || a->restore_ms != b->restore_ms) ? ICD4_CHG_RESTORE : 0u;
    c |= (a->alarms != b->alarms) ? ICD4_CHG_ALARMS : 0u;
    return c;
}

/* prepare link for use. */
void icd4_link_init(icd4_link_t *l)
{
    memset(l, 0, sizeof *l);
    l->stale = true;
}

/* take in one received controller frame; return true when it is valid. */
bool icd4_link_rx(icd4_link_t *l, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    icd4_ctrl_t f;
    icd4_err_t e = icd4_decode_ctrl(buf, len, &f);
    if (e != ICD4_OK) {
        if (e == ICD4_ERR_CRC) {
            l->crc_errors++;
        } else {
            l->length_errors++;
        }
        return false;
    }
    if (l->valid) {
        l->lost += (uint16_t)(f.seq - l->last.seq - 1u);
        l->changed |= icd4_changes(&l->last, &f);
    } else {
        l->changed = (uint8_t)(ICD4_CHG_SWITCH | ICD4_CHG_TRIP | ICD4_CHG_FLISR | ICD4_CHG_REASON | ICD4_CHG_RESTORE | ICD4_CHG_ALARMS);
    }
    l->last = f;
    l->valid = true;
    l->stale = false;
    l->last_rx_ms = now_ms;
    l->frames++;
    return true;
}

/* update stale state; called periodically. */
void icd4_link_tick(icd4_link_t *l, uint32_t now_ms)
{
    if (!l->valid || (uint32_t)(now_ms - l->last_rx_ms) >= ICD4_STALE_MS) {
        l->stale = true;
    }
}

/* return change flags not yet taken, and clear them. */
uint8_t icd4_link_take(icd4_link_t *l)
{
    uint8_t c = l->changed;
    l->changed = 0;
    return c;
}
