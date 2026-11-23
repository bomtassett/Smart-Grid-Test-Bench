/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-14
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (interface to 1D)
 * File:      Modbus RTU Slave, Frame Level (ICD-2, ICD-6).
 *
 * Purpose:   Implementation of Modbus slave described in modbus_slave.h.
 */
#include "modbus_slave.h"

/* function codes */
#define FC_READ_HOLDING    0x03u
#define FC_READ_INPUT      0x04u
#define FC_WRITE_SINGLE    0x06u
#define FC_WRITE_MULTIPLE  0x10u

/* return CRC-16 of block of bytes. */
uint16_t mb_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* return 16-bit value @ p, high byte first. */
static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* store 16-bit value @ p, high byte first. */
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/* append CRC to response; return frame length. */
static size_t finish(uint8_t *rsp, size_t len)
{
    uint16_t crc = mb_crc16(rsp, len);
    rsp[len]     = (uint8_t)(crc & 0xFFu);
    rsp[len + 1] = (uint8_t)(crc >> 8);
    return len + 2u;
}

/* build exception response; return frame length. */
static size_t exception(uint8_t *rsp, uint8_t addr, uint8_t fc, mb_ex_t ex, mb_stats_t *st)
{
    if (st) {
        st->exceptions++;
    }
    rsp[0] = addr;
    rsp[1] = (uint8_t)(fc | 0x80u);
    rsp[2] = (uint8_t)ex;
    return finish(rsp, 3u);
}

/* check that every reg of write range is mapped; return exception, if any. */
static mb_ex_t check_range(const mb_slave_t *s, uint16_t start, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++) {
        uint16_t dummy;
        mb_ex_t ex = s->read_holding(s->ctx, (uint16_t)(start + i), &dummy);
        if (ex != MB_EX_NONE) {
            return ex;
        }
    }
    return MB_EX_NONE;
}

/* process one received frame and build response; return response length, or 0 for no response. */
size_t mb_slave_process(const mb_slave_t *s, const uint8_t *req, size_t req_len,
                        uint8_t *rsp, size_t rsp_cap, mb_stats_t *st)
{
    if (s == NULL || req == NULL || rsp == NULL || rsp_cap < 8u) {
        return 0;
    }
    if (req_len < 4u || req_len > MB_MAX_ADU) {
        return 0;
    }
    uint16_t rx_crc = (uint16_t)(req[req_len - 2] | ((uint16_t)req[req_len - 1] << 8));
    if (mb_crc16(req, req_len - 2u) != rx_crc) {
        if (st) {
            st->crc_errors++;
        }
        return 0;
    }

    const uint8_t addr = req[0];
    const uint8_t fc = req[1];
    const int broadcast = (addr == MB_BROADCAST);
    if (!broadcast && addr != s->address) {
        if (st) {
            st->not_for_me++;
        }
        return 0;
    }

    const size_t pdu_len = req_len - 3u;   /* function code + data */
    size_t out = 0;

    switch (fc) {
    case FC_READ_HOLDING:
    case FC_READ_INPUT: {
        if (broadcast) {
            return 0;   /* broadcast read: not answered */
        }
        mb_read_fn rd = (fc == FC_READ_INPUT) ? s->read_input : s->read_holding;
        if (rd == NULL) {
            out = exception(rsp, addr, fc, MB_EX_ILLEGAL_FUNCTION, st);
            break;
        }
        if (pdu_len != 5u) {
            out = exception(rsp, addr, fc, MB_EX_ILLEGAL_VALUE, st);
            break;
        }
        uint16_t start = be16(&req[2]);
        uint16_t count = be16(&req[4]);
        if (count < 1u || count > MB_MAX_READ || rsp_cap < 5u + 2u * (size_t)count) {
            out = exception(rsp, addr, fc, MB_EX_ILLEGAL_VALUE, st);
            break;
        }
        if ((uint32_t)start + count > 0x10000u) {
            out = exception(rsp, addr, fc, MB_EX_ILLEGAL_ADDRESS, st);
            break;
        }
        if (s->latch) {
            s->latch(s->ctx);
        }
        mb_ex_t ex = MB_EX_NONE;
        for (uint16_t i = 0; i < count && ex == MB_EX_NONE; i++) {
            uint16_t v = 0;
            ex = rd(s->ctx, (uint16_t)(start + i), &v);
            put16(&rsp[3u + 2u * i], v);
        }
        if (ex != MB_EX_NONE) {
            out = exception(rsp, addr, fc, ex, st);
            break;
        }
        rsp[0] = addr;
        rsp[1] = fc;
        rsp[2] = (uint8_t)(2u * count);
        out = finish(rsp, 3u + 2u * (size_t)count);
        break;
    }

    case FC_WRITE_SINGLE: {
        if (s->write_holding == NULL || s->read_holding == NULL) {
            out = broadcast ? 0 : exception(rsp, addr, fc, MB_EX_ILLEGAL_FUNCTION, st);
            break;
        }
        if (pdu_len != 5u) {
            out = broadcast ? 0 : exception(rsp, addr, fc, MB_EX_ILLEGAL_VALUE, st);
            break;
        }
        uint16_t reg = be16(&req[2]);
        uint16_t val = be16(&req[4]);
        mb_ex_t ex = check_range(s, reg, 1u);
        if (ex == MB_EX_NONE) {
            ex = s->write_holding(s->ctx, reg, val);
        }
        if (ex != MB_EX_NONE) {
            out = broadcast ? 0 : exception(rsp, addr, fc, ex, st);
            break;
        }
        if (s->commit) {
            s->commit(s->ctx);
        }
        if (!broadcast) {
            /* response = echo of request */
            for (size_t i = 0; i < 6u; i++) {
                rsp[i] = req[i];
            }
            out = finish(rsp, 6u);
        }
        break;
    }

    case FC_WRITE_MULTIPLE: {
        if (s->write_holding == NULL || s->read_holding == NULL) {
            out = broadcast ? 0 : exception(rsp, addr, fc, MB_EX_ILLEGAL_FUNCTION, st);
            break;
        }
        if (pdu_len < 6u) {
            out = broadcast ? 0 : exception(rsp, addr, fc, MB_EX_ILLEGAL_VALUE, st);
            break;
        }
        uint16_t start = be16(&req[2]);
        uint16_t count = be16(&req[4]);
        uint8_t nbytes = req[6];
        if (count < 1u || count > MB_MAX_WRITE || nbytes != 2u * count ||
            pdu_len != 6u + (size_t)nbytes) {
            out = broadcast ? 0 : exception(rsp, addr, fc, MB_EX_ILLEGAL_VALUE, st);
            break;
        }
        if ((uint32_t)start + count > 0x10000u) {
            out = broadcast ? 0 : exception(rsp, addr, fc, MB_EX_ILLEGAL_ADDRESS, st);
            break;
        }
        mb_ex_t ex = check_range(s, start, count);
        for (uint16_t i = 0; i < count && ex == MB_EX_NONE; i++) {
            ex = s->write_holding(s->ctx, (uint16_t)(start + i), be16(&req[7u + 2u * i]));
        }
        if (ex != MB_EX_NONE) {
            out = broadcast ? 0 : exception(rsp, addr, fc, ex, st);
            break;
        }
        if (s->commit) {
            s->commit(s->ctx);
        }
        if (!broadcast) {
            for (size_t i = 0; i < 6u; i++) {
                rsp[i] = req[i];
            }
            out = finish(rsp, 6u);
        }
        break;
    }

    default:
        out = broadcast ? 0 : exception(rsp, addr, fc, MB_EX_ILLEGAL_FUNCTION, st);
        break;
    }

    if (st && out != 0u && (rsp[1] & 0x80u) == 0u) {
        st->frames_ok++;
    }
    return out;
}
