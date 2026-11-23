/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-20
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (input from 0A/0B)
 * File:      Node Sampling Engine.
 *
 * Purpose:   Measures voltage and current inputs of node and hands each measurement, in volts
 *            and amperes, to node application.
 *
 * Notes:     Scan: one conversion of every pair of node.
 *            Output sample: sum of several scans (oversampling).
 *            ADC1 + ADC2 are triggered together by TIM6, so V and I of pair are sampled @ same instant.
 *            Results are moved by DMA into circular buffer and processed in DMA irq.
 */
#include "board.h"
#include "stm32g4xx_ll_adc.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_dma.h"
#include "stm32g4xx_ll_dmamux.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_rcc.h"
#include "stm32g4xx_ll_tim.h"

/* settings of one pair. */
typedef struct {
    uint32_t     adc1_channel;   /* ADC1 channel */
    uint32_t     adc2_channel;   /* ADC2 channel */
    bool         v_on_adc1;      /* true = V input is on ADC1 */
    float        v_per_count;    /* line V per count */
    float        i_per_count;    /* line A per count */
    meter_kind_t kind;           /* AC or DC */
} pair_cfg_t;

#if defined(NODE_SRC)
/* pairs of source node: MP-01, MP-02 (DC), MP-03, MP-04. */
static const pair_cfg_t PAIRS[NODE_PAIRS] = {
    {LL_ADC_CHANNEL_1,  LL_ADC_CHANNEL_2,  true,  V_PER_COUNT(DIV_K12), I_PER_COUNT(0.01f), METER_AC},
    {LL_ADC_CHANNEL_15, LL_ADC_CHANNEL_17, false, V_PER_COUNT(DIV_K12), I_PER_COUNT(0.01f), METER_DC},
    {LL_ADC_CHANNEL_6,  LL_ADC_CHANNEL_7,  true,  V_PER_COUNT(DIV_K12), I_PER_COUNT(0.01f), METER_AC},
    {LL_ADC_CHANNEL_8,  LL_ADC_CHANNEL_9,  true,  V_PER_COUNT(DIV_K24), I_PER_COUNT(0.02f), METER_AC},
};
#else
/* pairs of switchyard node: 24 V side, then load feed. */
static const pair_cfg_t PAIRS[NODE_PAIRS] = {
    {LL_ADC_CHANNEL_1,  LL_ADC_CHANNEL_2,  true,  V_PER_COUNT(DIV_K24), I_PER_COUNT(0.05f), METER_AC},
    {LL_ADC_CHANNEL_15, LL_ADC_CHANNEL_17, false, V_PER_COUNT(DIV_K12), I_PER_COUNT(0.05f), METER_AC},
};
#endif

static const uint32_t RANKS[4] = {LL_ADC_REG_RANK_1, LL_ADC_REG_RANK_2, LL_ADC_REG_RANK_3, LL_ADC_REG_RANK_4};   /* scan positions */

#define HALF_WORDS  (ADC_OSR * NODE_PAIRS)   /* buffer words per output sample */
static uint32_t s_buf[2u * HALF_WORDS];      /* DMA buffer (two halves) */

/* mid-rail tracking: mid level of AC channel is taken as mean of its own samples. */
#define MID_NOMINAL  8192.0f   /* mid-rail level, nominal (counts) */
#define MID_SETTLED  4096u     /* samples before level is settled */
/* mid-rail tracker of one channel. */
typedef struct {
    float    mid;   /* tracked level (counts) */
    uint32_t n;     /* samples taken */
} mid_t;
static mid_t s_mid[NODE_PAIRS][2];   /* trackers, [pair][0 = V, 1 = I] */

/* remove mid-rail level from sample; return centred value. */
static inline float centre(mid_t *m, float x, bool track)
{
    if (track) {
        if (m->n < MID_SETTLED) {
            m->n++;
            m->mid += (x - m->mid) / (float)m->n;   /* running mean while settling */
        } else {
            m->mid += (x - m->mid) * (1.0f / (float)MID_SETTLED);
        }
    }
    return x - m->mid;
}

/* fill in kind (AC or DC) of each point. */
void sampling_kinds(meter_kind_t kinds[REG_MAX_POINTS])
{
    for (unsigned p = 0; p < REG_MAX_POINTS; p++) {
        kinds[p] = (p < NODE_PAIRS) ? PAIRS[p].kind : METER_AC;
    }
}

/* return tracked mid-rail level of channel (counts). */
float sampling_mid(unsigned pair, bool current)
{
    return (pair < NODE_PAIRS) ? s_mid[pair][current ? 1 : 0].mid : 0.0f;
}

/* turn one buffer half into one output sample of every pair. */
static void process(const uint32_t *b)
{
    /* sums taken first, so that buffer half is released early */
    uint32_t s1[NODE_PAIRS], s2[NODE_PAIRS];
    for (unsigned p = 0; p < NODE_PAIRS; p++) {
        uint32_t a1 = 0, a2 = 0;
        for (unsigned k = 0; k < ADC_OSR; k++) {
            uint32_t w = b[k * NODE_PAIRS + p];
            a1 += w & 0xFFFFu;
            a2 += w >> 16;
        }
        s1[p] = a1;
        s2[p] = a2;
    }
    for (unsigned p = 0; p < NODE_PAIRS; p++) {
        /* sum of conversions -> 14-bit sample */
        float c1 = (float)s1[p] * 0.25f, c2 = (float)s2[p] * 0.25f;
        const pair_cfg_t *c = &PAIRS[p];
        bool ac = (c->kind == METER_AC);
        float v = centre(&s_mid[p][0], c->v_on_adc1 ? c1 : c2, ac) * c->v_per_count;
        float i = centre(&s_mid[p][1], c->v_on_adc1 ? c2 : c1, ac) * c->i_per_count;
        node_app_sample(&g_node, (uint8_t)p, v, i);
    }
    g_diag.blocks++;
    LL_GPIO_TogglePin(GPIOC, LL_GPIO_PIN_8);   /* sample-rate test pin */
}

/* DMA irq: process each buffer half as it fills. */
void DMA1_Channel1_IRQHandler(void)
{
    bool ht = LL_DMA_IsActiveFlag_HT1(DMA1) != 0u;
    bool tc = LL_DMA_IsActiveFlag_TC1(DMA1) != 0u;
    if (ht && tc) {
        g_diag.overruns++;
    }
    if (ht) {
        LL_DMA_ClearFlag_HT1(DMA1);
        process(&s_buf[0]);
    }
    if (tc) {
        LL_DMA_ClearFlag_TC1(DMA1);
        process(&s_buf[HALF_WORDS]);
    }
}

/* wait for given time (us). */
static void spin_us(uint32_t us)
{
    for (volatile uint32_t n = us * (SYSCLK_HZ / 4000000u); n; n--) {
        __NOP();
    }
}

/* configure one converter. */
static void adc_setup(ADC_TypeDef *adc, bool master)
{
    LL_ADC_DisableDeepPowerDown(adc);
    LL_ADC_EnableInternalRegulator(adc);
    spin_us(LL_ADC_DELAY_INTERNAL_REGUL_STAB_US * 2u);

    LL_ADC_SetResolution(adc, LL_ADC_RESOLUTION_12B);
    LL_ADC_SetDataAlignment(adc, LL_ADC_DATA_ALIGN_RIGHT);
    LL_ADC_REG_SetContinuousMode(adc, LL_ADC_REG_CONV_SINGLE);
    LL_ADC_REG_SetOverrun(adc, LL_ADC_REG_OVR_DATA_OVERWRITTEN);
    LL_ADC_REG_SetSequencerLength(adc, (NODE_PAIRS == 4u) ? LL_ADC_REG_SEQ_SCAN_ENABLE_4RANKS : LL_ADC_REG_SEQ_SCAN_ENABLE_2RANKS);
    for (unsigned p = 0; p < NODE_PAIRS; p++) {
        uint32_t ch = master ? PAIRS[p].adc1_channel : PAIRS[p].adc2_channel;
        LL_ADC_REG_SetSequencerRanks(adc, RANKS[p], ch);
        LL_ADC_SetChannelSamplingTime(adc, ch, LL_ADC_SAMPLINGTIME_47CYCLES_5);
        LL_ADC_SetChannelSingleDiff(adc, ch, LL_ADC_SINGLE_ENDED);
    }
    if (master) {
        LL_ADC_REG_SetTriggerSource(adc, LL_ADC_REG_TRIG_EXT_TIM6_TRGO);
        LL_ADC_REG_SetTriggerEdge(adc, LL_ADC_REG_TRIG_EXT_RISING);
    } else {
        LL_ADC_REG_SetTriggerSource(adc, LL_ADC_REG_TRIG_SOFTWARE);   /* slave triggered w/ master */
    }

    LL_ADC_StartCalibration(adc, LL_ADC_SINGLE_ENDED);
    while (LL_ADC_IsCalibrationOnGoing(adc)) {
    }
    spin_us(2);
}

/* prepare sampling engine. */
void sampling_init(void)
{
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA | LL_AHB2_GRP1_PERIPH_GPIOB | LL_AHB2_GRP1_PERIPH_GPIOC);
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_ADC12);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1 | LL_AHB1_GRP1_PERIPH_DMAMUX1);
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
    LL_RCC_SetADCClockSource(LL_RCC_ADC12_CLKSOURCE_SYSCLK);

    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_0, LL_GPIO_MODE_ANALOG);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_1, LL_GPIO_MODE_ANALOG);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_4, LL_GPIO_MODE_ANALOG);
    LL_GPIO_SetPinMode(GPIOB, LL_GPIO_PIN_0, LL_GPIO_MODE_ANALOG);
#if NODE_PAIRS == 4u
    LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_0, LL_GPIO_MODE_ANALOG);
    LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_1, LL_GPIO_MODE_ANALOG);
    LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_2, LL_GPIO_MODE_ANALOG);
    LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_3, LL_GPIO_MODE_ANALOG);
#endif
    LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_8, LL_GPIO_MODE_OUTPUT);   /* sample-rate test pin */

    for (unsigned p = 0; p < NODE_PAIRS; p++) {
        s_mid[p][0].mid = s_mid[p][1].mid = MID_NOMINAL;
        s_mid[p][0].n = s_mid[p][1].n = 0;
    }

    /* converter clock synchronous w/ timer */
    LL_ADC_SetCommonClock(__LL_ADC_COMMON_INSTANCE(ADC1), LL_ADC_CLOCK_SYNC_PCLK_DIV4);
    adc_setup(ADC1, true);
    adc_setup(ADC2, false);
    LL_ADC_SetMultimode(__LL_ADC_COMMON_INSTANCE(ADC1), LL_ADC_MULTI_DUAL_REG_SIMULT);
    LL_ADC_SetMultiDMATransfer(__LL_ADC_COMMON_INSTANCE(ADC1), LL_ADC_MULTI_REG_DMA_UNLMT_RES12_10B);

    LL_DMA_SetPeriphRequest(DMA1, LL_DMA_CHANNEL_1, LL_DMAMUX_REQ_ADC1);
    LL_DMA_ConfigTransfer(DMA1, LL_DMA_CHANNEL_1,
                          LL_DMA_DIRECTION_PERIPH_TO_MEMORY | LL_DMA_MODE_CIRCULAR | LL_DMA_PERIPH_NOINCREMENT |
                          LL_DMA_MEMORY_INCREMENT | LL_DMA_PDATAALIGN_WORD | LL_DMA_MDATAALIGN_WORD | LL_DMA_PRIORITY_VERYHIGH);
    LL_DMA_ConfigAddresses(DMA1, LL_DMA_CHANNEL_1, LL_ADC_DMA_GetRegAddr(ADC1, LL_ADC_DMA_REG_REGULAR_DATA_MULTI),
                           (uint32_t)s_buf, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
    LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, 2u * HALF_WORDS);
    LL_DMA_EnableIT_HT(DMA1, LL_DMA_CHANNEL_1);
    LL_DMA_EnableIT_TC(DMA1, LL_DMA_CHANNEL_1);
    NVIC_SetPriority(DMA1_Channel1_IRQn, PRIO_SAMPLING);

    LL_TIM_SetPrescaler(TIM6, 0);
    LL_TIM_SetAutoReload(TIM6, ADC_TRIG_DIV - 1u);
    LL_TIM_SetTriggerOutput(TIM6, LL_TIM_TRGO_UPDATE);
}

/* start sampling. */
void sampling_start(void)
{
    LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);
    NVIC_EnableIRQ(DMA1_Channel1_IRQn);

    LL_ADC_Enable(ADC1);
    while (!LL_ADC_IsActiveFlag_ADRDY(ADC1)) {
    }
    LL_ADC_Enable(ADC2);
    while (!LL_ADC_IsActiveFlag_ADRDY(ADC2)) {
    }
    LL_ADC_REG_StartConversion(ADC1);   /* one scan per TIM6 update */
    LL_TIM_EnableCounter(TIM6);
}
