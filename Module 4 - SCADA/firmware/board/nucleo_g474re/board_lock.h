/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-20
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers
 * File:      Node Firmware Register Lock.
 *
 * Purpose:   Stops sampling interrupt from changing published values while they are being copied
 *            for Modbus reply, so that reply is never built from half-updated set.
 *
 * Notes:     Forced into every board source w/ -include.
 *            The RS-485 rx irq is not held off by lock.
 *            Lock may be nested.
 */
#ifndef BOARD_LOCK_H
#define BOARD_LOCK_H
#ifndef __ASSEMBLER__
void board_lock(void);     /* take register lock */
void board_unlock(void);   /* release register lock */
#define NODE_REGS_LOCK()    board_lock()
#define NODE_REGS_UNLOCK()  board_unlock()
#endif
#endif
