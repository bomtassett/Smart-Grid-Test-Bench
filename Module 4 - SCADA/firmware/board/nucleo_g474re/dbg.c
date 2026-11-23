/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-20
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers
 * File:      Node Bring-up Console.
 *
 * Purpose:   Prints status text to PC through board's USB debug port (ST-LINK virtual COM port),
 *            for use during bring-up. Output only.
 *
 * Notes:     LPUART1, PA2 TX.
 *            Ring: buffer of chars waiting to be sent; text that does not fit is dropped.
 */
#include "board.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_lpuart.h"

#define RING 2048u                         /* ring size (chars) */
static uint8_t           s_ring[RING];     /* ring */
static volatile uint32_t s_head, s_tail;   /* write position, read position */

/* send next char of ring. */
void LPUART1_IRQHandler(void)
{
    if (LL_LPUART_IsEnabledIT_TXE_TXFNF(LPUART1) && LL_LPUART_IsActiveFlag_TXE_TXFNF(LPUART1)) {
        if (s_tail != s_head) {
            LL_LPUART_TransmitData8(LPUART1, s_ring[s_tail & (RING - 1u)]);
            s_tail++;
        } else {
            LL_LPUART_DisableIT_TXE_TXFNF(LPUART1);
        }
    }
}

/* prepare console. */
void dbg_init(void)
{
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_LPUART1);
    LL_GPIO_SetAFPin_0_7(GPIOA, LL_GPIO_PIN_2, LL_GPIO_AF_12);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_2, LL_GPIO_MODE_ALTERNATE);

    LL_LPUART_SetTransferDirection(LPUART1, LL_LPUART_DIRECTION_TX);
    LL_LPUART_ConfigCharacter(LPUART1, LL_LPUART_DATAWIDTH_8B, LL_LPUART_PARITY_NONE, LL_LPUART_STOPBITS_1);
    LL_LPUART_SetBaudRate(LPUART1, SYSCLK_HZ, LL_LPUART_PRESCALER_DIV1, 115200u);
    LL_LPUART_Enable(LPUART1);
    NVIC_SetPriority(LPUART1_IRQn, PRIO_DEBUG);
    NVIC_EnableIRQ(LPUART1_IRQn);
}

/* queue one char. */
static void put(char c)
{
    if ((uint32_t)(s_head - s_tail) >= RING) {
        return;   /* ring full */
    }
    s_ring[s_head & (RING - 1u)] = (uint8_t)c;
    s_head++;
}

/* print string. */
void dbg_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            put('\r');
        }
        put(*s++);
    }
    LL_LPUART_EnableIT_TXE_TXFNF(LPUART1);
}

/* print unsigned integer. */
void dbg_u32(uint32_t v)
{
    char b[11];
    unsigned n = sizeof b;
    b[--n] = '\0';
    do {
        b[--n] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && n);
    dbg_puts(&b[n]);
}

/* print number w/ given decimals. */
void dbg_fixed(float v, unsigned decimals)
{
    if (v != v) {
        dbg_puts("nan");
        return;
    }
    if (v < 0.0f) {
        dbg_puts("-");
        v = -v;
    }
    if (v > 4.0e9f) {
        dbg_puts("ovf");
        return;
    }
    float scale = 1.0f;
    for (unsigned d = 0; d < decimals; d++) {
        scale *= 10.0f;
    }
    uint32_t whole = (uint32_t)v;
    uint32_t frac = (uint32_t)((v - (float)whole) * scale + 0.5f);
    if (frac >= (uint32_t)scale) {
        frac -= (uint32_t)scale;
        whole++;
    }
    dbg_u32(whole);
    if (decimals) {
        dbg_puts(".");
        for (uint32_t div = (uint32_t)scale / 10u; div; div /= 10u) {
            char c[2] = {(char)('0' + (frac / div) % 10u), '\0'};
            dbg_puts(c);
        }
    }
}
