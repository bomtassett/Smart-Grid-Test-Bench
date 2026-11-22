/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-14
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (simulated)
 * File:      Frame-level Simulated Slave.
 *
 * Purpose:   Minimal stand-in for one node or for load control unit that answers Modbus requests
 *            typed as text. Used to check that host's tag table and firmware's register bank agree.
 *
 * Usage:     mbsim node <address> <npoints>     measurement node w/ fixed values
 *            mbsim lcu                          load control unit stand-in
 *
 * Notes:     stdin: one request per line, as hex bytes. stdout: response as hex; "-" = no response.
 *            Node values, point p: VRMS 12.000 + p V, IRMS 0.1000 * (p + 1) A, FREQ 60.000 Hz,
 *              ANG -1.00 * p deg, PF 0.990, P 1.500 * (p + 1) W, Q -0.250 * p var,
 *              S 1.600 * (p + 1) VA, E 1000 * (p + 1) mWh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modbus_slave.h"
#include "node_regs.h"
#include "lcu_regs.h"

/* answer requests read from stdin. */
int main(int argc, char **argv)
{
    mb_slave_t s;
    node_regs_t node;
    lcu_regs_t lcu = {0};
    int is_lcu = (argc >= 2 && strcmp(argv[1], "lcu") == 0);

    if (is_lcu) {
        lcu_regs_bind(&lcu, &s);
    } else if (argc == 4 && strcmp(argv[1], "node") == 0) {
        uint8_t np = (uint8_t)atoi(argv[3]);
        node_regs_init(&node, np, 0x0001);
        node_regs_bind(&node, &s, (uint8_t)atoi(argv[2]));
        node.live.status = HLTH_STATUS_SAMPLING | HLTH_STATUS_CAL_VALID;
        node.live.time_ms = 123456;
        for (uint8_t p = 0; p < node.npoints; p++) {
            node_meas_t m = {12.0f + p, 0.1f * (p + 1), 60.0f, -1.0f * p, 0.99f,
                             1.5f * (p + 1), -0.25f * p, 1.6f * (p + 1)};
            node_regs_set_point(&node, p, &m);
            node_regs_set_energy_mwh(&node, p, 1000u * (p + 1u));
        }
    } else {
        fprintf(stderr, "usage: mbsim node <address> <npoints> | mbsim lcu\n");
        return 2;
    }

    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        uint8_t req[MB_MAX_ADU], rsp[MB_MAX_ADU];
        size_t n = 0;
        for (char *t = strtok(line, " \t\r\n"); t && n < sizeof req; t = strtok(NULL, " \t\r\n")) {
            req[n++] = (uint8_t)strtoul(t, NULL, 16);
        }
        size_t m = mb_slave_process(&s, req, n, rsp, sizeof rsp, NULL);
        if (is_lcu) {
            lcu.contacts = lcu.shed;   /* contacts follow command */
            lcu.seq++;
        }
        if (m == 0) {
            puts("-");
        } else {
            for (size_t i = 0; i < m; i++) {
                printf("%02X%s", rsp[i], i + 1 < m ? " " : "\n");
            }
        }
        fflush(stdout);
    }
    return 0;
}
