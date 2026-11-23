/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-20
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers
 * File:      Node System Clock.
 *
 * Purpose:   Sets processor clock from board's crystal. When crystal does not start,
 *            internal oscillator (HSI) is used instead.
 */
#include "board.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_pwr.h"
#include "stm32g4xx_ll_rcc.h"
#include "stm32g4xx_ll_system.h"

/* wait for n passes of loop. */
static void spin(volatile uint32_t n)
{
    while (n--) {
        __NOP();
    }
}

/* set system clock; return true when crystal is in use. */
bool clock_init(void)
{
    bool hse = true;

    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);

    LL_FLASH_SetLatency(LL_FLASH_LATENCY_4);
    while (LL_FLASH_GetLatency() != LL_FLASH_LATENCY_4) {
    }
    LL_PWR_EnableRange1BoostMode();   /* boost mode, for full clock rate */

    /* crystal preferred: node's frequency accuracy rests on it (CAP-M4-03) */
    LL_RCC_HSE_Enable();
    for (uint32_t t = 0; !LL_RCC_HSE_IsReady(); t++) {
        if (t > 2000000u) {
            hse = false;
            break;
        }
    }
    if (hse) {
        LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSE, LL_RCC_PLLM_DIV_6, 85, LL_RCC_PLLR_DIV_2);
    } else {
        LL_RCC_HSE_Disable();
        LL_RCC_HSI_Enable();
        while (!LL_RCC_HSI_IsReady()) {
        }
        LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSI, LL_RCC_PLLM_DIV_4, 85, LL_RCC_PLLR_DIV_2);
    }
    LL_RCC_PLL_EnableDomain_SYS();
    LL_RCC_PLL_Enable();
    while (!LL_RCC_PLL_IsReady()) {
    }

    /* bus clock halved while system clock is raised */
    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_2);
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL) {
    }
    spin(200);
    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
    LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);

    SystemCoreClock = SYSCLK_HZ;
    return hse;
}
