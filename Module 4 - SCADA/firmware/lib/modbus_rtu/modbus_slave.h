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
 * Purpose:   Lets device answer Modbus master: received request is checked, registers it names
 *            are read or written, and reply is built.
 *
 * Notes:     Master: device that sends requests (host). Slave: device that answers them.
 *            Frame: one complete Modbus message, from slave address through CRC.
 *            Reg: register. FC: function code. Exception: error reply of slave.
 */
#ifndef MODBUS_SLAVE_H
#define MODBUS_SLAVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB_MAX_ADU       256u   /* largest frame (bytes) */
#define MB_MAX_READ      125u   /* most regs in one read */
#define MB_MAX_WRITE     123u   /* most regs in one write */
#define MB_BROADCAST     0u     /* broadcast address */

/* exception codes. */
typedef enum {
    MB_EX_NONE             = 0,
    MB_EX_ILLEGAL_FUNCTION = 1,
    MB_EX_ILLEGAL_ADDRESS  = 2,
    MB_EX_ILLEGAL_VALUE    = 3,
    MB_EX_DEVICE_FAILURE   = 4
} mb_ex_t;

/* callbacks that read and write one reg. */
typedef mb_ex_t (*mb_read_fn)(void *ctx, uint16_t addr, uint16_t *value);
typedef mb_ex_t (*mb_write_fn)(void *ctx, uint16_t addr, uint16_t value);

/* description of one slave. */
typedef struct {
    uint8_t     address;              /* slave address */
    mb_read_fn  read_input;           /* read input reg */
    mb_read_fn  read_holding;         /* read holding reg */
    mb_write_fn write_holding;        /* write holding reg */
    void      (*latch)(void *ctx);    /* called before read is answered (optional) */
    void      (*commit)(void *ctx);   /* called after write is applied (optional) */
    void       *ctx;                  /* context passed to callbacks */
} mb_slave_t;

/* frame counters. */
typedef struct {
    uint32_t frames_ok;    /* frames answered */
    uint32_t crc_errors;   /* frames w/ bad CRC */
    uint32_t exceptions;   /* exceptions returned */
    uint32_t not_for_me;   /* frames for another slave */
} mb_stats_t;

/* return CRC-16 of block of bytes. */
uint16_t mb_crc16(const uint8_t *data, size_t len);

/* process one received frame and build response; return response length, or 0 for no response. */
size_t mb_slave_process(const mb_slave_t *s, const uint8_t *req, size_t req_len,
                        uint8_t *rsp, size_t rsp_cap, mb_stats_t *stats);

/* return high word (word 0) or low word (word 1) of 32-bit value. */
static inline uint16_t mb_u32_word(uint32_t v, unsigned word)
{
    return (uint16_t)(word == 0u ? (v >> 16) : (v & 0xFFFFu));
}

/* join two words into 32-bit value. */
static inline uint32_t mb_u32_join(uint16_t hi, uint16_t lo)
{
    return ((uint32_t)hi << 16) | lo;
}

#ifdef __cplusplus
}
#endif
#endif   /* MODBUS_SLAVE_H */
