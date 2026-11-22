/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-21
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (test)
 * File:      Tests: ICD-4 Frames & Link Supervision.
 *
 * Purpose:   Checks, on PC w/ no hardware, that controller link messages survive encoding and decoding
 *            unchanged, that damaged message is rejected, and that lost or silent link is detected.
 */
#include <stdio.h>
#include <string.h>
#include "icd4.h"
#include "modbus_slave.h"

static int fails, checks;   /* failed checks, checks run */
/* count one check; print failure. */
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* run tests. */
int main(void)
{
    uint8_t b[64];
    icd4_ctrl_t c = {0x1234, 0x89ABCDEFu, 0x0005, 2, ICD4_FAULT_SHORT, 3, {1.05f, 0.02f, 0.0f}, 11, 2, 612u, 0x10}, c2;
    icd4_node_t n = {0xBEEF, 5000250u, 23.98f, 0.444f, 10.65f, 5.21f, 0x0013}, n2;

    /* controller frame: 32 bytes, order of Table ICD-4.3 */
    CHECK(icd4_encode_ctrl(&c, b) == ICD4_CTRL_LEN);
    CHECK(b[0] == 0x34 && b[1] == 0x12 && b[2] == 0xEF && b[5] == 0x89 && b[6] == 0x05 && b[7] == 0x00);
    CHECK(b[8] == 2 && b[9] == ICD4_FAULT_SHORT && b[10] == 3 && b[23] == 11 && b[24] == 2);
    CHECK(b[25] == 0x64 && b[26] == 0x02 && b[27] == 0 && b[28] == 0 && b[29] == 0x10);
    CHECK(mb_crc16(b, 30) == (uint16_t)(b[30] | (b[31] << 8)) && mb_crc16(b, 32) == 0);
    CHECK(icd4_decode_ctrl(b, 32, &c2) == ICD4_OK);
    CHECK(c2.seq == c.seq && c2.t_ms == c.t_ms && c2.sw_state == c.sw_state && c2.trip_evt == 2 && c2.fault_type == ICD4_FAULT_SHORT);
    CHECK(c2.flisr_state == 3 && c2.i_prot[0] == 1.05f && c2.i_prot[1] == 0.02f && c2.i_prot[2] == 0.0f);
    CHECK(c2.reason_code == 11 && c2.donor_id == 2 && c2.restore_ms == 612u && c2.alarms == 0x10);
    CHECK(icd4_decode_ctrl(b, 31, &c2) == ICD4_ERR_LENGTH && icd4_decode_ctrl(b, 26, &c2) == ICD4_ERR_LENGTH);
    for (int k = 0; k < 32; k++) {   /* single corrupted byte anywhere is caught */
        b[k] ^= 0x40;
        CHECK(icd4_decode_ctrl(b, 32, &c2) == ICD4_ERR_CRC);
        b[k] ^= 0x40;
    }

    /* node frame: 26 bytes, order of Table ICD-4.4 */
    CHECK(icd4_encode_node(&n, b) == ICD4_NODE_LEN && mb_crc16(b, 26) == 0);
    CHECK(b[0] == 0xEF && b[1] == 0xBE && b[22] == 0x13 && b[23] == 0x00);
    CHECK(icd4_decode_node(b, 26, &n2) == ICD4_OK);
    CHECK(n2.seq == n.seq && n2.t_ms == n.t_ms && n2.v_sub == n.v_sub && n2.i_sub == n.i_sub && n2.p_sub == n.p_sub && n2.p_load == n.p_load && n2.health == n.health);
    CHECK(icd4_decode_node(b, 32, &n2) == ICD4_ERR_LENGTH);
    b[7] ^= 1;
    CHECK(icd4_decode_node(b, 26, &n2) == ICD4_ERR_CRC);

    /* link: first frame, heartbeat, state change, loss, stale */
    icd4_link_t l;
    icd4_link_init(&l);
    CHECK(l.stale && !l.valid);
    icd4_ctrl_t f = {100, 0, 0x0003, 0, ICD4_FAULT_NONE, 0, {0.3f, 0.3f, 0.0f}, 12, 0, 0, 0};
    icd4_encode_ctrl(&f, b);
    CHECK(icd4_link_rx(&l, b, 32, 1000) && l.valid && !l.stale && l.frames == 1);
    CHECK(icd4_link_take(&l) == 0x3F && icd4_link_take(&l) == 0);   /* everything is reported once by first frame */
    f.seq = 101;
    icd4_encode_ctrl(&f, b);
    CHECK(icd4_link_rx(&l, b, 32, 1100) && icd4_link_take(&l) == 0 && l.lost == 0);   /* heartbeat only: nothing changed */
    f.seq = 102; f.sw_state = 0x0000; f.trip_evt = 1; f.fault_type = ICD4_FAULT_SHORT; f.reason_code = 0;
    icd4_encode_ctrl(&f, b);
    CHECK(icd4_link_rx(&l, b, 32, 1130));
    CHECK(icd4_link_take(&l) == (ICD4_CHG_SWITCH | ICD4_CHG_TRIP | ICD4_CHG_REASON));
    f.seq = 105; f.reason_code = 11; f.donor_id = 2; f.restore_ms = 612; f.flisr_state = 4; f.alarms = 1;
    icd4_encode_ctrl(&f, b);
    CHECK(icd4_link_rx(&l, b, 32, 1400) && l.lost == 2);
    CHECK(icd4_link_take(&l) == (ICD4_CHG_FLISR | ICD4_CHG_REASON | ICD4_CHG_RESTORE | ICD4_CHG_ALARMS));
    b[3] ^= 0xFF;
    CHECK(!icd4_link_rx(&l, b, 32, 1500) && l.crc_errors == 1 && l.frames == 4);
    CHECK(!icd4_link_rx(&l, b, 12, 1500) && l.length_errors == 1);
    icd4_link_tick(&l, 1699);
    CHECK(!l.stale);
    icd4_link_tick(&l, 1700);
    CHECK(l.stale && l.last.reason_code == 11);   /* stale after 300 ms; last state is kept */
    f.seq = 0;                                    /* sequence number wraps */
    l.last.seq = 0xFFFF;
    icd4_encode_ctrl(&f, b);
    uint32_t lost = l.lost;
    CHECK(icd4_link_rx(&l, b, 32, 1800) && !l.stale && l.lost == lost);

    printf("test_icd4: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
