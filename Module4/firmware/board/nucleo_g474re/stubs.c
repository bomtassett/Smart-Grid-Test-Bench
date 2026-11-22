/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-20
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers
 * File:      Node Firmware C Library Stubs.
 *
 * Purpose:   Empty versions of four system calls that C library refers to and firmware never
 *            uses (there are no files and no standard streams on board).
 */
/* unused system calls. */
int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int offset, int whence) { (void)fd; (void)offset; (void)whence; return -1; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return -1; }
int _write(int fd, const char *buf, int len) { (void)fd; (void)buf; return len; }
