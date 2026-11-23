/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-14
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (interface to 1D)
 * File:      Load Control Unit Register Bank (ICD-6).
 *
 * Purpose:   Small set of Modbus registers through which host commands load relays open or
 *            closed, and reads back their actual states, any faults, and heartbeat.
 *
 * Notes:     LCU: load control unit.
 *            Contact: one load relay. Shed: commanded open.
 *            Header-only, for use in Module 3 controller sketch.
 */
#ifndef LCU_REGS_H
#define LCU_REGS_H

#include <stdbool.h>
#include <stdint.h>
#include "modbus_slave.h"
#include "regmap_lcu.h"

/* register bank of LCU. */
typedef struct {
    uint16_t shed;           /* commanded contacts (bit i = 1: contact i open) */
    uint16_t contacts;       /* actual contact states */
    uint16_t fault;          /* fault flags */
    uint16_t seq;            /* heartbeat */
    bool     shed_changed;   /* true = shed command was written */
} lcu_regs_t;

/* read one input reg. */
static mb_ex_t lcu_read_input(void *ctx, uint16_t a, uint16_t *v)
{
    const lcu_regs_t *r = (const lcu_regs_t *)ctx;
    switch (a) {
    case LCU_IR_STAT_CONTACTS: *v = r->contacts; return MB_EX_NONE;
    case LCU_IR_STAT_FAULT:    *v = r->fault;    return MB_EX_NONE;
    case LCU_IR_HLTH_SEQ:      *v = r->seq;      return MB_EX_NONE;
    default:                   return MB_EX_ILLEGAL_ADDRESS;
    }
}

/* read one holding reg. */
static mb_ex_t lcu_read_holding(void *ctx, uint16_t a, uint16_t *v)
{
    const lcu_regs_t *r = (const lcu_regs_t *)ctx;
    if (a != LCU_HR_CTRL_SHED) {
        return MB_EX_ILLEGAL_ADDRESS;
    }
    *v = r->shed;
    return MB_EX_NONE;
}

/* write one holding reg. */
static mb_ex_t lcu_write_holding(void *ctx, uint16_t a, uint16_t v)
{
    lcu_regs_t *r = (lcu_regs_t *)ctx;
    if (a != LCU_HR_CTRL_SHED) {
        return MB_EX_ILLEGAL_ADDRESS;
    }
    if (v & (uint16_t)~LCU_CONTACT_MASK) {
        return MB_EX_ILLEGAL_VALUE;   /* bit beyond last contact */
    }
    if (v != r->shed) {
        r->shed = v;
        r->shed_changed = true;
    }
    return MB_EX_NONE;
}

/* fill in slave description that serves this register bank. */
static inline void lcu_regs_bind(lcu_regs_t *r, mb_slave_t *s)
{
    s->address = REG_ADDR_LCU;
    s->read_input = lcu_read_input;
    s->read_holding = lcu_read_holding;
    s->write_holding = lcu_write_holding;
    s->latch = 0;
    s->commit = 0;
    s->ctx = r;
}

#endif   /* LCU_REGS_H */
