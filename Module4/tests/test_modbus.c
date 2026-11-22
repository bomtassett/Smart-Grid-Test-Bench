/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-14
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (test)
 * File:      Unit Tests: Modbus Slave & Register Banks.
 *
 * Purpose:   Checks, on PC w/ no hardware, that Modbus requests are answered correctly by slave
 *            and by both register banks (node and load control unit).
 *
 * Notes:     Covered: CRC, FC 03/04/06/16, exceptions, latching, float32 word order, LCU bank.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "modbus_slave.h"
#include "node_regs.h"
#include "lcu_regs.h"

static int fails, checks;   /* failed checks, checks run */
/* count one check; print failure. */
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static size_t frame(uint8_t *f, size_t n)   /* CRC is appended to n bytes */
{
    uint16_t crc = mb_crc16(f, n);
    f[n] = (uint8_t)(crc & 0xFF);
    f[n + 1] = (uint8_t)(crc >> 8);
    return n + 2;
}

/* build read request; return its length. */
static size_t req_read(uint8_t *f, uint8_t addr, uint8_t fc, uint16_t start, uint16_t count)
{
    f[0] = addr; f[1] = fc; f[2] = (uint8_t)(start >> 8); f[3] = (uint8_t)start;
    f[4] = (uint8_t)(count >> 8); f[5] = (uint8_t)count;
    return frame(f, 6);
}

static uint16_t reg(const uint8_t *rsp, unsigned i)   /* i-th reg of read response */
{
    return (uint16_t)((rsp[3 + 2 * i] << 8) | rsp[4 + 2 * i]);
}

/* test CRC. */
static void test_crc(void)
{
    /* reference frames from Modbus serial-line literature */
    const uint8_t a[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};   /* CRC C5 CD */
    const uint8_t b[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x02};   /* CRC 71 CB */
    CHECK(mb_crc16(a, sizeof a) == 0xCDC5);
    CHECK(mb_crc16(b, sizeof b) == 0xCB71);
    CHECK(mb_crc16((const uint8_t *)"123456789", 9) == 0x4B37);   /* CRC-16/MODBUS check value */
}

/* return float32 from two regs of response, high word first. */
static float regf(const uint8_t *p, unsigned i)
{
    uint32_t u = mb_u32_join(reg(p, i), reg(p, i + 1));
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* test node register bank over Modbus. */
static void test_node(void)
{
    node_regs_t r;
    mb_slave_t s;
    mb_stats_t st = {0};
    uint8_t q[MB_MAX_ADU], p[MB_MAX_ADU];
    size_t n;

    node_regs_init(&r, REG_SY1_POINTS, 0x0001);
    node_regs_bind(&r, &s, REG_ADDR_SY1);

    r.live.status = HLTH_STATUS_SAMPLING | HLTH_STATUS_CAL_VALID;
    r.live.seq = 7;
    r.live.time_ms = 0x00012345u;
    node_meas_t sub  = {24.123f, 0.2725f, 60.002f, -1.25f, 0.999f, 6.571f, -0.143f, 6.573f};
    node_meas_t load = {12.034f, 0.5450f, 59.998f, 0.00f, 1.000f, 6.559f, 0.000f, 6.559f};
    node_regs_set_point(&r, PT_SY1_SUB, &sub);
    node_regs_set_point(&r, PT_SY1_LOAD, &load);
    node_regs_set_energy_mwh(&r, PT_SY1_LOAD, 1093);

    /* 10 Hz read: one FC 04 request from IR 0 */
    n = mb_slave_process(&s, q, req_read(q, REG_ADDR_SY1, 0x04, 0, REG_SY1_FAST_READ), p, sizeof p, &st);
    CHECK(n == 5u + 2u * REG_SY1_FAST_READ);
    CHECK(p[0] == REG_ADDR_SY1 && p[1] == 0x04 && p[2] == 2u * REG_SY1_FAST_READ);
    CHECK(mb_crc16(p, n) == 0);   /* frame followed by its own CRC gives CRC of 0 */
    CHECK(reg(p, IR_HLTH_STATUS) == (HLTH_STATUS_SAMPLING | HLTH_STATUS_CAL_VALID));
    CHECK(reg(p, IR_HLTH_SEQ) == 7);
    CHECK(mb_u32_join(reg(p, IR_HLTH_TIME), reg(p, IR_HLTH_TIME + 1)) == 0x00012345u);
    /* reg pair holds float32 itself, bit for bit: no scale, no rounding */
    CHECK(regf(p, IR_POINT(PT_SY1_SUB, PT_OFF_VRMS)) == 24.123f);
    CHECK(regf(p, IR_POINT(PT_SY1_SUB, PT_OFF_IRMS)) == 0.2725f);
    CHECK(regf(p, IR_POINT(PT_SY1_SUB, PT_OFF_FREQ)) == 60.002f);
    CHECK(regf(p, IR_POINT(PT_SY1_SUB, PT_OFF_ANG)) == -1.25f);
    CHECK(regf(p, IR_POINT(PT_SY1_SUB, PT_OFF_PF)) == 0.999f);
    CHECK(regf(p, IR_POINT(PT_SY1_SUB, PT_OFF_P)) == 6.571f);
    CHECK(regf(p, IR_POINT(PT_SY1_SUB, PT_OFF_Q)) == -0.143f);
    CHECK(regf(p, IR_POINT(PT_SY1_SUB, PT_OFF_S)) == 6.573f);
    CHECK(regf(p, IR_POINT(PT_SY1_LOAD, PT_OFF_VRMS)) == 12.034f);
    CHECK(regf(p, IR_POINT(PT_SY1_LOAD, PT_OFF_IRMS)) == 0.5450f);
    /* word order: 24.123 = 41C0FBE7h; high word is @ lower address */
    CHECK(reg(p, IR_POINT(PT_SY1_SUB, PT_OFF_VRMS)) == 0x41C0 && reg(p, IR_POINT(PT_SY1_SUB, PT_OFF_VRMS) + 1) == 0xFBE7);
    /* 10 Hz read is header + point blocks, w/ no unused reg */
    CHECK(REG_SY1_FAST_READ == 4u + 16u * REG_SY1_POINTS && IR_POINT(0, 0) == IR_HLTH_TIME + 2u);

    /* 1 Hz read: fw version (IR 80), one reserved reg (IR 81), energy counters (IR 82 on) */
    n = mb_slave_process(&s, q, req_read(q, REG_ADDR_SY1, 0x04, IR_SLOW_BASE, REG_SY1_SLOW_READ), p, sizeof p, &st);
    CHECK(n == 5u + 2u * REG_SY1_SLOW_READ && REG_SY1_SLOW_READ == 2u + 2u * REG_SY1_POINTS);
    CHECK(reg(p, IR_HLTH_FWVER - IR_SLOW_BASE) == 0x0001 && reg(p, 1) == 0);
    CHECK(mb_u32_join(reg(p, IR_ENERGY(PT_SY1_LOAD) - IR_SLOW_BASE), reg(p, IR_ENERGY(PT_SY1_LOAD) - IR_SLOW_BASE + 1)) == 1093);

    /* latching: value changed between two requests is seen only in second */
    r.live.seq = 8;
    CHECK(r.latched.seq == 7);

    /* no range limit: 150 W, 10 A of reflected fault current, and 1 mW load are all held as given */
    node_meas_t big = {30.0f, 10.0f, 60.0f, 0.0f, 1.0f, 150.0f, 0.0f, 150.0f};
    node_regs_set_point(&r, PT_SY1_SUB, &big);
    CHECK(r.live.pt[PT_SY1_SUB].v[PT_FIELD(PT_OFF_P)] == 150.0f && r.live.pt[PT_SY1_SUB].v[PT_FIELD(PT_OFF_IRMS)] == 10.0f);
    big.p = 0.001f;
    node_regs_set_point(&r, PT_SY1_SUB, &big);
    CHECK(r.live.pt[PT_SY1_SUB].v[PT_FIELD(PT_OFF_P)] == 0.001f && !(r.live.status & (HLTH_STATUS_FAULT | HLTH_STATUS_OVERRANGE)));
    /* value that is not finite is read as 0 and FAULT is set;
     * FAULT is cleared once latest set of every point is finite */
    big.q = NAN;
    node_regs_set_point(&r, PT_SY1_SUB, &big);
    CHECK(r.live.pt[PT_SY1_SUB].v[PT_FIELD(PT_OFF_Q)] == 0.0f && (r.live.status & HLTH_STATUS_FAULT));
    big.q = INFINITY;
    node_regs_set_point(&r, PT_SY1_LOAD, &big);
    big.q = 0.0f;
    node_regs_set_point(&r, PT_SY1_SUB, &big);
    CHECK(r.live.status & HLTH_STATUS_FAULT);   /* other point still holds bad set */
    node_regs_set_point(&r, PT_SY1_LOAD, &load);
    CHECK(!(r.live.status & HLTH_STATUS_FAULT) && (r.live.status & HLTH_STATUS_SAMPLING));

    /* unmapped IRs: exception 02 is returned between blocks and after slow block */
    n = mb_slave_process(&s, q, req_read(q, REG_ADDR_SY1, 0x04, IR_SLOW_BASE + REG_SY1_SLOW_READ, 1), p, sizeof p, &st);
    CHECK(n == 5 && p[1] == 0x84 && p[2] == MB_EX_ILLEGAL_ADDRESS);
    n = mb_slave_process(&s, q, req_read(q, REG_ADDR_SY1, 0x04, 60, 2), p, sizeof p, &st);
    CHECK(n == 5 && p[1] == 0x84 && p[2] == MB_EX_ILLEGAL_ADDRESS);
    /* point block this node does not have (SY1: two points, not four) */
    n = mb_slave_process(&s, q, req_read(q, REG_ADDR_SY1, 0x04, 0, REG_SRC_FAST_READ), p, sizeof p, &st);
    CHECK(n == 5 && p[2] == MB_EX_ILLEGAL_ADDRESS);
    /* count of zero or oversize count: exception 03 */
    n = mb_slave_process(&s, q, req_read(q, REG_ADDR_SY1, 0x04, 0, 0), p, sizeof p, &st);
    CHECK(n == 5 && p[2] == MB_EX_ILLEGAL_VALUE);
    n = mb_slave_process(&s, q, req_read(q, REG_ADDR_SY1, 0x04, 0, 126), p, sizeof p, &st);
    CHECK(n == 5 && p[2] == MB_EX_ILLEGAL_VALUE);
    /* unsupported function: exception 01 */
    q[0] = REG_ADDR_SY1; q[1] = 0x2B; q[2] = 0x0E; q[3] = 0x01;
    n = mb_slave_process(&s, q, frame(q, 4), p, sizeof p, &st);
    CHECK(n == 5 && p[1] == 0xAB && p[2] == MB_EX_ILLEGAL_FUNCTION);

    /* no response: another slave's address, corrupted CRC, short frame */
    CHECK(mb_slave_process(&s, q, req_read(q, REG_ADDR_SY2, 0x04, 0, 5), p, sizeof p, &st) == 0);
    n = req_read(q, REG_ADDR_SY1, 0x04, 0, 5);
    q[n - 1] ^= 0x01;
    CHECK(mb_slave_process(&s, q, n, p, sizeof p, &st) == 0);
    CHECK(mb_slave_process(&s, q, 3, p, sizeof p, &st) == 0);
    CHECK(st.crc_errors == 1 && st.not_for_me == 1);

    /* FC 16: calibration of channel 3 (point 1, current) is written, then read back w/ FC 03 */
    const float gain = 1.0123f, offset = -0.0045f;
    uint32_t g, o;
    memcpy(&g, &gain, 4);
    memcpy(&o, &offset, 4);
    q[0] = REG_ADDR_SY1; q[1] = 0x10;
    q[2] = (uint8_t)(HR_CAL_GAIN(3) >> 8); q[3] = (uint8_t)HR_CAL_GAIN(3);
    q[4] = 0; q[5] = 4; q[6] = 8;
    q[7] = (uint8_t)(g >> 24); q[8] = (uint8_t)(g >> 16); q[9] = (uint8_t)(g >> 8); q[10] = (uint8_t)g;
    q[11] = (uint8_t)(o >> 24); q[12] = (uint8_t)(o >> 16); q[13] = (uint8_t)(o >> 8); q[14] = (uint8_t)o;
    n = mb_slave_process(&s, q, frame(q, 15), p, sizeof p, &st);
    CHECK(n == 8 && p[1] == 0x10 && p[5] == 4);
    CHECK(r.cal_dirty);
    CHECK(node_regs_cal_gain(&r, 3) == gain && node_regs_cal_offset(&r, 3) == offset);
    CHECK(node_regs_cal_gain(&r, 2) == 1.0f);   /* neighbouring channel is untouched */
    n = mb_slave_process(&s, q, req_read(q, REG_ADDR_SY1, 0x03, HR_CAL_GAIN(3), 4), p, sizeof p, &st);
    CHECK(n == 13 && mb_u32_join(reg(p, 0), reg(p, 1)) == g && mb_u32_join(reg(p, 2), reg(p, 3)) == o);

    /* FC 16 write past mapped range changes nothing */
    uint16_t before = r.ctrl;
    q[0] = REG_ADDR_SY1; q[1] = 0x10; q[2] = 0; q[3] = (uint8_t)HR_CTRL; q[4] = 0; q[5] = 2; q[6] = 4;
    q[7] = 0; q[8] = 1; q[9] = 0; q[10] = 1;
    n = mb_slave_process(&s, q, frame(q, 11), p, sizeof p, &st);
    CHECK(n == 5 && p[1] == 0x90 && p[2] == MB_EX_ILLEGAL_ADDRESS && r.ctrl == before);

    /* FC 06: control word, energy reset, value checks */
    q[0] = REG_ADDR_SY1; q[1] = 0x06; q[2] = 0; q[3] = (uint8_t)HR_CTRL; q[4] = 0; q[5] = CTRL_SAMPLE_EN;
    n = mb_slave_process(&s, q, frame(q, 6), p, sizeof p, &st);
    CHECK(n == 8 && memcmp(p, q, 8) == 0 && r.ctrl == CTRL_SAMPLE_EN && r.ctrl_dirty);
    q[5] = 0x80;   /* undefined control bit */
    n = mb_slave_process(&s, q, frame(q, 6), p, sizeof p, &st);
    CHECK(n == 5 && p[2] == MB_EX_ILLEGAL_VALUE && r.ctrl == CTRL_SAMPLE_EN);
    q[3] = (uint8_t)HR_ENERGY_RESET; q[5] = 0x02;   /* reset point 1 */
    n = mb_slave_process(&s, q, frame(q, 6), p, sizeof p, &st);
    CHECK(n == 8 && r.energy_reset_pending == 0x02);
    q[5] = 0x04;   /* SY1 has no point 2 */
    n = mb_slave_process(&s, q, frame(q, 6), p, sizeof p, &st);
    CHECK(n == 5 && p[2] == MB_EX_ILLEGAL_VALUE);

    /* broadcast time set: applied, and not answered */
    q[0] = MB_BROADCAST; q[1] = 0x10; q[2] = 0; q[3] = (uint8_t)HR_TIME_SET; q[4] = 0; q[5] = 2; q[6] = 4;
    q[7] = 0x00; q[8] = 0x0F; q[9] = 0x42; q[10] = 0x40;   /* 1 000 000 ms */
    CHECK(mb_slave_process(&s, q, frame(q, 11), p, sizeof p, &st) == 0);
    CHECK(r.time_set_pending && node_regs_time_set(&r) == 1000000u);
    /* broadcast read is ignored */
    CHECK(mb_slave_process(&s, q, req_read(q, MB_BROADCAST, 0x04, 0, 5), p, sizeof p, &st) == 0);
}

/* test LCU register bank over Modbus. */
static void test_lcu(void)
{
    lcu_regs_t r = {0};
    mb_slave_t s;
    uint8_t q[MB_MAX_ADU], p[MB_MAX_ADU];
    size_t n;
    lcu_regs_bind(&r, &s);

    /* FC 06 to LCU.CTRL.SHED (HR 1): contacts 1 and 3 are commanded open; echo is acknowledgment */
    q[0] = REG_ADDR_LCU; q[1] = 0x06; q[2] = 0; q[3] = LCU_HR_CTRL_SHED; q[4] = 0; q[5] = 0x0A;
    n = mb_slave_process(&s, q, frame(q, 6), p, sizeof p, NULL);
    CHECK(n == 8 && memcmp(p, q, 8) == 0);
    CHECK(r.shed == 0x000A && r.shed_changed);

    /* relays are moved and reported by application */
    r.contacts = r.shed;
    r.seq = 42;
    n = mb_slave_process(&s, q, req_read(q, REG_ADDR_LCU, 0x04, LCU_IR_FIRST, LCU_IR_COUNT), p, sizeof p, NULL);
    CHECK(n == 11 && reg(p, 0) == 0x000A && reg(p, 1) == 0 && reg(p, 2) == 42);

    /* bit beyond eight contacts: write is refused and nothing changes */
    q[0] = REG_ADDR_LCU; q[1] = 0x06; q[2] = 0; q[3] = LCU_HR_CTRL_SHED; q[4] = 0x01; q[5] = 0x00;
    n = mb_slave_process(&s, q, frame(q, 6), p, sizeof p, NULL);
    CHECK(n == 5 && p[1] == 0x86 && p[2] == MB_EX_ILLEGAL_VALUE && r.shed == 0x000A);
    /* IR 0 is not mapped (ICD-6 map starts @ IR 1) */
    n = mb_slave_process(&s, q, req_read(q, REG_ADDR_LCU, 0x04, 0, 3), p, sizeof p, NULL);
    CHECK(n == 5 && p[2] == MB_EX_ILLEGAL_ADDRESS);
}

/* run tests. */
int main(void)
{
    test_crc();
    test_node();
    test_lcu();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
