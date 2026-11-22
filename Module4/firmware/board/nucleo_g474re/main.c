/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-20
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers
 * File:      Measurement Node Firmware, NUCLEO-G474RE.
 *
 * Purpose:   Main program of measurement node. @ start-up clock is set, stored calibration
 *            and energy are restored, and sampling is started. Work is then shared between three
 *            interrupts and main loop:
 *              USART3 irq     chars of Modbus frame are received
 *              DMA1 Ch1 irq   one output sample of every point is processed
 *              SysTick irq    node time is advanced
 *              main loop      frames are answered, host writes acted on, state saved, status printed
 */
#include "board.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_gpio.h"

node_app_t   g_node;   /* node application */
board_diag_t g_diag;   /* diagnostic counters */

static mb_slave_t        s_slave;             /* Modbus slave description */
static mb_stats_t        s_stats;             /* Modbus frame counters */
static uint8_t           s_rsp[MB_MAX_ADU];   /* response buffer */
static volatile uint32_t s_ms;                /* time since start-up (ms) */
static volatile uint32_t s_lock_depth;        /* nesting depth of register lock */

/* ------------------------------------------------------------ register lock */
/* take register lock. */
void board_lock(void)
{
    __set_BASEPRI(PRIO_SAMPLING << (8u - __NVIC_PRIO_BITS));
    s_lock_depth++;
}

/* release register lock. */
void board_unlock(void)
{
    if (--s_lock_depth == 0u) {
        __set_BASEPRI(0u);
    }
}

/* ---------------------------------------------------------------- time base */
/* 1 ms tick: advance time base and node time. */
void SysTick_Handler(void)
{
    s_ms++;
    node_app_tick_1ms(&g_node);
}

/* return time since start-up (ms). */
uint32_t board_millis(void)
{
    return s_ms;
}

/* stop node on hard fault. */
void HardFault_Handler(void)
{
    __disable_irq();
    LL_GPIO_ResetOutputPin(GPIOB, LL_GPIO_PIN_5);   /* bus released */
    for (;;) {
        LL_GPIO_TogglePin(GPIOA, LL_GPIO_PIN_5);
        for (volatile uint32_t n = 0; n < 1500000u; n++) {
        }
    }
}

/* ------------------------------------------------------------------ console */
/* print start-up banner. */
static void banner(unsigned nv)
{
    dbg_puts("\nSGTB Module 4 measurement node  " NODE_NAME "  address ");
    dbg_u32(NODE_ADDRESS);
    dbg_puts("  firmware ");
    dbg_u32(NODE_FWVER >> 8);
    dbg_puts(".");
    dbg_u32(NODE_FWVER & 0xFFu);
    dbg_puts("\nclock: ");
    dbg_puts(g_diag.hse ? "24 MHz crystal" : "INTERNAL OSCILLATOR (crystal did not start; frequency accuracy is not met)");
    dbg_puts("\nsample rate: ");
    dbg_fixed(FS_HZ, 2);
    dbg_puts(" Hz, points: ");
    dbg_u32(NODE_PAIRS);
    dbg_puts("\nstore: ");
    if (!(nv & NV_AVAILABLE)) {
        dbg_puts("DISABLED (flash is not in the dual-bank layout, or the store failed)");
    } else {
        dbg_puts((nv & NV_CAL_LOADED) ? "calibration restored" : "no calibration stored, nominal scaling in use");
        dbg_puts((nv & NV_ENERGY_LOADED) ? ", energy restored" : ", energy starts at zero");
    }
    dbg_puts("\n");
}

/* print status report. */
static void report(void)
{
    for (unsigned p = 0; p < NODE_PAIRS; p++) {
        meter_result_t r;
        board_lock();
        r = g_node.meter[p].out;
        board_unlock();
        dbg_puts("p");
        dbg_u32(p);
        dbg_puts(" V ");
        dbg_fixed(r.vrms, 3);
        dbg_puts(" I ");
        dbg_fixed(r.irms, 4);
        dbg_puts(" f ");
        dbg_fixed(r.freq, 3);
        dbg_puts(" P ");
        dbg_fixed(r.p, 3);
        dbg_puts(" pf ");
        dbg_fixed(r.pf, 3);
        dbg_puts(" mid ");
        dbg_fixed(sampling_mid(p, false), 1);
        dbg_puts(" ");
        dbg_fixed(sampling_mid(p, true), 1);
        dbg_puts("\n");
    }
    dbg_puts("t ");
    dbg_u32(g_node.time_ms);
    dbg_puts(" samples ");
    dbg_u32(g_diag.blocks);
    dbg_puts(" overruns ");
    dbg_u32(g_diag.overruns);
    dbg_puts(" mb ok ");
    dbg_u32(s_stats.frames_ok);
    dbg_puts(" other ");
    dbg_u32(s_stats.not_for_me);
    dbg_puts(" crc ");
    dbg_u32(s_stats.crc_errors);
    dbg_puts(" exc ");
    dbg_u32(s_stats.exceptions);
    dbg_puts(" line ");
    dbg_u32(g_diag.rx_errors);
    dbg_puts(" status 0x");
    {
        static const char hex[] = "0123456789ABCDEF";
        uint16_t st = g_node.regs.live.status;
        char b[5] = {hex[(st >> 12) & 15u], hex[(st >> 8) & 15u], hex[(st >> 4) & 15u], hex[st & 15u], '\0'};
        dbg_puts(b);
    }
    dbg_puts("\n");
}

/* --------------------------------------------------------------------- main */
/* run start-up, then main loop. */
int main(void)
{
    g_diag.hse = clock_init();

    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_OUTPUT);   /* LD2 */
    dbg_init();

    meter_cfg_t cfg;
    meter_kind_t kinds[REG_MAX_POINTS];
    meter_default_cfg(&cfg);
    cfg.fs = FS_HZ;
    sampling_kinds(kinds);
    node_app_init(&g_node, NODE_PAIRS, kinds, &cfg);
    node_regs_bind(&g_node.regs, &s_slave, NODE_ADDRESS);

    unsigned nv = nv_init(&g_node);
    if (!(nv & NV_CAL_LOADED)) {
        g_node.regs.live.status &= (uint16_t)~HLTH_STATUS_CAL_VALID;   /* no stored calibration */
    }
    banner(nv);

    sampling_init();
    rs485_init();
    SysTick_Config(SYSCLK_HZ / 1000u);
    NVIC_SetPriority(SysTick_IRQn, PRIO_TICK);
    sampling_start();

    uint32_t t_led = 0, t_report = 0;
    for (;;) {
        const uint8_t *req;
        size_t len;
        if (rs485_frame(&req, &len)) {
            size_t n = mb_slave_process(&s_slave, req, len, s_rsp, sizeof s_rsp, &s_stats);
            if (n > 0u) {
                rs485_send(s_rsp, n);
            } else {
                rs485_release();
            }
        }

        /* host writes */
        board_lock();
        node_app_service(&g_node);
        board_unlock();

        uint32_t now = board_millis();
        nv_task(&g_node, now);

        if ((uint32_t)(now - t_led) >= (g_diag.hse ? 500u : 100u)) {   /* LD2 blink period */
            t_led = now;
            LL_GPIO_TogglePin(GPIOA, LL_GPIO_PIN_5);
        }
        if ((uint32_t)(now - t_report) >= 1000u) {
            t_report = now;
            report();
        }
    }
}
