/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-14
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (simulated)
 * File:      Simulated Measurement Nodes & Load Control Unit.
 *
 * Purpose:   Stands in for node hardware on PC, so that host software can be developed and tested
 *            before any board exists. Simulated nodes answer on virtual serial ports exactly as real
 *            nodes would, since firmware's own code (meter, node_app, node_regs, modbus_slave) is run,
 *            fed w/ synthetic 3840 Hz samples in real time.
 *            Two stand-alone test circuits of testing plan are modelled:
 *              12 V circuit   source -> MP-01 shunt -> MP-29 shunt -> test load
 *              24 V circuit   source -> MP-04 shunt -> 1 ohm test resistor -> MP-21 shunt -> test load
 *              MP-03          voltage input only (function generator, Case 4)
 *
 * Usage:     simnode [--seg1 LINK] [--seg2 LINK] [--nodes N] [--ideal]
 *              --seg1 LINK   symlink for node fieldbus port (default /tmp/sgtb-seg1)
 *              --seg2 LINK   symlink for load-control port  (default /tmp/sgtb-seg2)
 *              --nodes N     nodes on seg 1, 1..5 (default 2: NODE-SRC, NODE-SY1)
 *              --ideal       no converter quantization or noise
 *
 * Notes:     stdin commands, one per line:
 *              load open|low|medium|high|<ohms>     12 V test load (low 96, medium 49, high 22)
 *              load24 open|<ohms>                   24 V test load (96 or 49 in plan)
 *              pf <0..1>                            lagging power factor, 12 V load
 *              freq <Hz>                            source frequency
 *              volt <V12> <V24>                     source voltages, rms
 *              sync <Vrms> <Hz>                     MP-03 voltage input (0 0 = disconnected)
 *              offline <addr> | online <addr>       slave address stops or resumes answering
 *              status                               present settings are printed
 *              quit
 */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "lcu_regs.h"
#include "modbus_slave.h"
#include "node_app.h"

#define FS 3840.0   /* sample rate (Hz) */
#define PI 3.14159265358979323846
#define MAX_NODES 5   /* most nodes */

/* one virtual serial port. */
typedef struct {
    int      fd, keep;          /* file descriptors: master side, slave side */
    uint8_t  buf[MB_MAX_ADU];   /* frame being received */
    size_t   len;               /* length of that frame */
    double   last_rx;           /* time of last char received (s) */
} port_t;

/* settings of simulated bench. */
typedef struct {
    double v12, v24, freq, r12, r24, pf, sync_v, sync_f;   /* source voltages (V), frequency (Hz), load resistances (ohm), power factor, MP-03 voltage (V) and frequency (Hz) */
    int    ideal;                                          /* true = no converter model */
} bench_t;

static node_app_t g_node[MAX_NODES];    /* node applications */
static mb_slave_t g_slave[MAX_NODES];   /* slave descriptions of nodes */
static int        g_offline[256];       /* true = slave address does not answer */
static int        g_nodes = 2;          /* number of nodes */
static lcu_regs_t g_lcu;                /* LCU register bank */
static mb_slave_t g_lcu_slave;          /* slave description of LCU */
static bench_t    g_b = {12.0, 24.0, 60.0, 22.0, 49.0, 1.0, 0.0, 60.0, 0};   /* bench settings */

/* return time (s). */
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* open virtual serial port and link it @ given path. */
static int open_port(port_t *p, const char *link)
{
    struct termios tio;
    p->fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->fd < 0 || grantpt(p->fd) != 0 || unlockpt(p->fd) != 0) {
        perror("pty");
        return -1;
    }
    const char *name = ptsname(p->fd);
    p->keep = open(name, O_RDWR | O_NOCTTY);
    if (p->keep < 0 || tcgetattr(p->keep, &tio) != 0) {
        perror("pty slave");
        return -1;
    }
    cfmakeraw(&tio);
    tcsetattr(p->keep, TCSANOW, &tio);
    unlink(link);
    if (symlink(name, link) != 0) {
        perror(link);
        return -1;
    }
    p->len = 0;
    printf("%s -> %s\n", link, name);
    return 0;
}

/* converter model: return x w/ quantization and noise. */
static float adc(double x, double lsb)
{
    if (g_b.ideal) {
        return (float)x;
    }
    double noise = ((double)rand() / RAND_MAX * 2.0 - 1.0) * lsb;
    return (float)(floor((x + noise) / lsb + 0.5) * lsb);
}

/* converter step sizes (V or A per count) */
#define LSB_V12 0.0032
#define LSB_V24 0.0062
#define LSB_I_LOAD 0.0002      /* load feed */
#define LSB_I_SUBMAIN 0.0005   /* main substation */
#define LSB_I_SRC 0.0010       /* source */

/* feed one sample of every point to every node. */
static void bench_sample(double th, double ths)
{
    const double r2 = 1.41421356237309505;
    double phi = acos(g_b.pf);
    double i12 = (g_b.r12 > 0.0) ? g_b.v12 / g_b.r12 : 0.0;           /* rms */
    double i24 = (g_b.r24 > 0.0) ? g_b.v24 / (g_b.r24 + 1.0) : 0.0;   /* rms */
    double v12 = r2 * g_b.v12 * sin(th);
    double v24 = r2 * g_b.v24 * sin(th);
    double c12 = r2 * i12 * sin(th - phi);
    double c24 = r2 * i24 * sin(th);
    double v21 = v24 - 1.0 * c24;   /* voltage after test resistor */

    /* NODE-SRC: MAINS (MP-01), PV (MP-02, DC, unused), SYNC (MP-03), SUBMAIN (MP-04) */
    node_app_sample(&g_node[0], PT_SRC_MAINS, adc(v12, LSB_V12), adc(c12, LSB_I_SRC));
    node_app_sample(&g_node[0], PT_SRC_PV, adc(0.0, LSB_V12), adc(0.0, LSB_I_SRC));
    node_app_sample(&g_node[0], PT_SRC_SYNC, adc(r2 * g_b.sync_v * sin(ths), LSB_V12), adc(0.0, LSB_I_SRC));
    node_app_sample(&g_node[0], PT_SRC_SUBMAIN, adc(v24, LSB_V24), adc(c24, LSB_I_SUBMAIN));
    /* NODE-SY1: SUB (MP-21), LOAD (MP-29) */
    if (g_nodes > 1) {
        node_app_sample(&g_node[1], PT_SY1_SUB, adc(v21, LSB_V24), adc(c24, LSB_I_LOAD));
        node_app_sample(&g_node[1], PT_SY1_LOAD, adc(v12, LSB_V12), adc(c12, LSB_I_LOAD));
    }
    /* further switchyard nodes: energized, no load */
    for (int n = 2; n < g_nodes; n++) {
        node_app_sample(&g_node[n], 0, adc(v24, LSB_V24), adc(0.0, LSB_I_LOAD));
        node_app_sample(&g_node[n], 1, adc(v12, LSB_V12), adc(0.0, LSB_I_LOAD));
    }
}

/* answer frame held by port. */
static void serve(port_t *p, int segment)
{
    uint8_t rsp[MB_MAX_ADU];
    size_t n = 0;
    if (segment == 1) {
        for (int k = 0; k < g_nodes && n == 0; k++) {
            if (!g_offline[g_slave[k].address]) {
                n = mb_slave_process(&g_slave[k], p->buf, p->len, rsp, sizeof rsp, NULL);
                node_app_service(&g_node[k]);
            }
        }
    } else if (!g_offline[REG_ADDR_LCU]) {
        n = mb_slave_process(&g_lcu_slave, p->buf, p->len, rsp, sizeof rsp, NULL);
        g_lcu.contacts = g_lcu.shed;   /* contacts follow command */
        if (g_lcu.shed_changed) {
            g_lcu.shed_changed = false;
            printf("LCU shed command 0x%02X\n", g_lcu.shed);
        }
    }
    p->len = 0;
    if (n > 0 && write(p->fd, rsp, n) < 0) {
        perror("write");
    }
}

/* read chars from port; answer frame once it has ended. */
static void pump(port_t *p, int segment, double t)
{
    uint8_t tmp[256];
    ssize_t n = read(p->fd, tmp, sizeof tmp);
    if (n > 0) {
        if (p->len + (size_t)n > sizeof p->buf) {
            p->len = 0;   /* overrun */
        } else {
            memcpy(p->buf + p->len, tmp, (size_t)n);
            p->len += (size_t)n;
        }
        p->last_rx = t;
    }
    /* frame ended by silent interval */
    if (p->len > 0 && t - p->last_rx >= 0.002) {
        serve(p, segment);
    }
}

/* print present settings. */
static void print_status(void)
{
    printf("12 V circuit: %.2f V, load %s%.1f ohm, PF %.3f | 24 V circuit: %.2f V, load %s%.1f ohm | %.3f Hz | MP-03 %.2f V %.2f Hz\n",
           g_b.v12, g_b.r12 > 0 ? "" : "open ", g_b.r12, g_b.pf, g_b.v24, g_b.r24 > 0 ? "" : "open ", g_b.r24,
           g_b.freq, g_b.sync_v, g_b.sync_f);
}

/* run one stdin command. */
static int command(char *line)
{
    char a[32] = "", b[32] = "", c[32] = "";
    int n = sscanf(line, "%31s %31s %31s", a, b, c);
    if (n < 1) {
        return 0;
    }
    if (!strcmp(a, "quit")) {
        return 1;
    } else if (!strcmp(a, "load") && n >= 2) {
        g_b.r12 = !strcmp(b, "open") ? 0.0 : !strcmp(b, "low") ? 96.0 : !strcmp(b, "medium") ? 49.0 :
                  !strcmp(b, "high") ? 22.0 : atof(b);
    } else if (!strcmp(a, "load24") && n >= 2) {
        g_b.r24 = !strcmp(b, "open") ? 0.0 : atof(b);
    } else if (!strcmp(a, "pf") && n >= 2) {
        double v = atof(b);
        g_b.pf = (v > 0.0 && v <= 1.0) ? v : 1.0;
    } else if (!strcmp(a, "freq") && n >= 2) {
        g_b.freq = atof(b);
    } else if (!strcmp(a, "volt") && n >= 3) {
        g_b.v12 = atof(b);
        g_b.v24 = atof(c);
    } else if (!strcmp(a, "sync") && n >= 3) {
        g_b.sync_v = atof(b);
        g_b.sync_f = atof(c);
    } else if ((!strcmp(a, "offline") || !strcmp(a, "online")) && n >= 2) {
        g_offline[atoi(b) & 0xFF] = !strcmp(a, "offline");
    } else if (strcmp(a, "status")) {
        printf("unknown command: %s\n", a);
        return 0;
    }
    print_status();
    return 0;
}

/* run simulator. */
int main(int argc, char **argv)
{
    const char *l1 = "/tmp/sgtb-seg1", *l2 = "/tmp/sgtb-seg2";
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "--seg1") && k + 1 < argc) {
            l1 = argv[++k];
        } else if (!strcmp(argv[k], "--seg2") && k + 1 < argc) {
            l2 = argv[++k];
        } else if (!strcmp(argv[k], "--nodes") && k + 1 < argc) {
            g_nodes = atoi(argv[++k]);
        } else if (!strcmp(argv[k], "--ideal")) {
            g_b.ideal = 1;
        } else {
            fprintf(stderr, "usage: simnode [--seg1 LINK] [--seg2 LINK] [--nodes N] [--ideal]\n");
            return 2;
        }
    }
    if (g_nodes < 1 || g_nodes > MAX_NODES) {
        fprintf(stderr, "--nodes must be 1 to %d\n", MAX_NODES);
        return 2;
    }

    const meter_kind_t src_kinds[4] = {METER_AC, METER_DC, METER_AC, METER_AC};
    node_app_init(&g_node[0], REG_SRC_POINTS, src_kinds, NULL);
    node_regs_bind(&g_node[0].regs, &g_slave[0], REG_ADDR_SRC);
    for (int n = 1; n < g_nodes; n++) {
        node_app_init(&g_node[n], 2, NULL, NULL);
        node_regs_bind(&g_node[n].regs, &g_slave[n], (uint8_t)(REG_ADDR_SY1 + (unsigned)n - 1u));
    }
    lcu_regs_bind(&g_lcu, &g_lcu_slave);

    port_t p1, p2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (open_port(&p1, l1) != 0 || open_port(&p2, l2) != 0) {
        return 1;
    }
    printf("simnode: %d node(s) on segment 1, load control unit at address %u on segment 2\n", g_nodes, REG_ADDR_LCU);
    print_status();

    double t_prev = now_s(), th = 0.0, ths = 0.0, sample_debt = 0.0, ms_debt = 0.0;
    char line[128];
    size_t ll = 0;
    int quit = 0, stdin_open = 1;
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);

    while (!quit) {
        struct pollfd fds[3] = {{p1.fd, POLLIN, 0}, {p2.fd, POLLIN, 0}, {stdin_open ? STDIN_FILENO : -1, POLLIN, 0}};
        poll(fds, 3, 1);
        double t = now_s();
        double dt = t - t_prev;
        t_prev = t;
        if (dt > 0.5) {
            dt = 0.5;   /* replay limited after stall */
        }
        for (sample_debt += dt * FS; sample_debt >= 1.0; sample_debt -= 1.0) {
            bench_sample(th, ths);
            th = fmod(th + 2.0 * PI * g_b.freq / FS, 2.0 * PI);
            ths = fmod(ths + 2.0 * PI * g_b.sync_f / FS, 2.0 * PI);
            g_lcu.seq++;
        }
        for (ms_debt += dt * 1000.0; ms_debt >= 1.0; ms_debt -= 1.0) {
            for (int n = 0; n < g_nodes; n++) {
                node_app_tick_1ms(&g_node[n]);
            }
        }
        pump(&p1, 1, t);
        pump(&p2, 2, t);

        char ch;
        ssize_t r = -1;
        while (stdin_open && (r = read(STDIN_FILENO, &ch, 1)) == 1) {
            if (ch == '\n' || ll + 1 >= sizeof line) {
                line[ll] = '\0';
                ll = 0;
                quit |= command(line);
            } else {
                line[ll++] = ch;
            }
        }
        if (r == 0) {
            stdin_open = 0;   /* stdin closed */
        }
    }
    unlink(l1);
    unlink(l2);
    return 0;
}
