/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-20
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers (interface to 1D)
 * File:      Node RS-485 Driver, Seg 1 (ICD-2).
 *
 * Purpose:   Receives and sends Modbus frames on RS-485 bus shared by nodes and host.
 *            Bus is half duplex: node listens, and drives line only while reply is sent.
 *
 * Notes:     USART3: PB10 TX, PB11 RX, PB5 driver enable.
 *            Driver: transmit enable of transceiver.
 *            End of frame: found by receiver timeout of USART.
 */
#include <string.h>
#include "board.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_usart.h"

#define RS485_BAUD        115200u   /* bit rate */
#define RS485_IDLE_BITS   202u      /* receiver timeout (bit times) */

/* driver state. */
typedef enum { LISTEN, READY, SENDING } state_t;

static volatile state_t s_state;              /* state */
static uint8_t          s_rx[MB_MAX_ADU];     /* received frame */
static volatile size_t  s_rx_len;             /* length of received frame */
static volatile bool    s_rx_bad;             /* true = received frame has line error */
static uint8_t          s_tx[MB_MAX_ADU];     /* frame being sent */
static volatile size_t  s_tx_len, s_tx_pos;   /* length, position of frame being sent */

/* switch driver on or off. */
static inline void driver(bool on)
{
    if (on) {
        LL_GPIO_SetOutputPin(GPIOB, LL_GPIO_PIN_5);
    } else {
        LL_GPIO_ResetOutputPin(GPIOB, LL_GPIO_PIN_5);
    }
}

/* return to listening. */
static void listen(void)
{
    LL_USART_RequestRxDataFlush(USART3);
    LL_USART_ClearFlag_ORE(USART3);
    LL_USART_ClearFlag_FE(USART3);
    LL_USART_ClearFlag_NE(USART3);
    LL_USART_ClearFlag_RTO(USART3);
    s_rx_len = 0;
    s_rx_bad = false;
    s_state = LISTEN;
}

/* receive and send chars; end frame on receiver timeout. */
void USART3_IRQHandler(void)
{
    uint32_t isr = USART3->ISR;

    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
        USART3->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
        /* line error: frame is marked bad; error w/ no char is dropped */
        if ((isr & USART_ISR_RXNE) || s_rx_len > 0u) {
            s_rx_bad = true;
        }
    }
    if (isr & USART_ISR_RXNE) {
        uint8_t b = (uint8_t)USART3->RDR;
        if (s_state == LISTEN) {
            if (s_rx_len < sizeof s_rx) {
                s_rx[s_rx_len++] = b;
            } else {
                s_rx_bad = true;
            }
        }
    }
    if (isr & USART_ISR_RTOF) {
        USART3->ICR = USART_ICR_RTOCF;
        if (s_state == LISTEN) {
            if (s_rx_len > 0u && !s_rx_bad) {
                s_state = READY;   /* frame complete */
            } else {
                if (s_rx_len > 0u) {
                    g_diag.rx_errors++;
                }
                s_rx_len = 0;
                s_rx_bad = false;
            }
        }
    }
    if (s_state == SENDING) {
        uint32_t cr1 = USART3->CR1;
        if ((cr1 & USART_CR1_TXEIE) && (isr & USART_ISR_TXE)) {
            USART3->TDR = s_tx[s_tx_pos++];
            if (s_tx_pos >= s_tx_len) {
                LL_USART_DisableIT_TXE_TXFNF(USART3);
                LL_USART_EnableIT_TC(USART3);
            }
        } else if ((cr1 & USART_CR1_TCIE) && (isr & USART_ISR_TC)) {
            LL_USART_DisableIT_TC(USART3);
            driver(false);
            listen();   /* echo of reply discarded */
        }
    }
}

/* prepare RS-485 driver. */
void rs485_init(void)
{
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOB);
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART3);

    driver(false);
    LL_GPIO_SetPinMode(GPIOB, LL_GPIO_PIN_5, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetPinOutputType(GPIOB, LL_GPIO_PIN_5, LL_GPIO_OUTPUT_PUSHPULL);

    LL_GPIO_SetAFPin_8_15(GPIOB, LL_GPIO_PIN_10, LL_GPIO_AF_7);
    LL_GPIO_SetAFPin_8_15(GPIOB, LL_GPIO_PIN_11, LL_GPIO_AF_7);
    LL_GPIO_SetPinSpeed(GPIOB, LL_GPIO_PIN_10, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_SetPinPull(GPIOB, LL_GPIO_PIN_11, LL_GPIO_PULL_UP);   /* idle level held w/o transceiver */
    LL_GPIO_SetPinMode(GPIOB, LL_GPIO_PIN_10, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinMode(GPIOB, LL_GPIO_PIN_11, LL_GPIO_MODE_ALTERNATE);

    LL_USART_SetTransferDirection(USART3, LL_USART_DIRECTION_TX_RX);
    LL_USART_ConfigCharacter(USART3, LL_USART_DATAWIDTH_8B, LL_USART_PARITY_NONE, LL_USART_STOPBITS_1);
    LL_USART_SetBaudRate(USART3, SYSCLK_HZ, LL_USART_PRESCALER_DIV1, LL_USART_OVERSAMPLING_16, RS485_BAUD);
    LL_USART_SetRxTimeout(USART3, RS485_IDLE_BITS);
    LL_USART_EnableRxTimeout(USART3);
    LL_USART_Enable(USART3);

    listen();
    LL_USART_EnableIT_RXNE_RXFNE(USART3);
    LL_USART_EnableIT_RTO(USART3);
    NVIC_SetPriority(USART3_IRQn, PRIO_RS485);
    NVIC_EnableIRQ(USART3_IRQn);
}

/* return true, and frame, when complete frame is waiting. */
bool rs485_frame(const uint8_t **frame, size_t *len)
{
    if (s_state != READY) {
        return false;
    }
    *frame = s_rx;
    *len = s_rx_len;
    return true;
}

/* drop frame and resume listening. */
void rs485_release(void)
{
    NVIC_DisableIRQ(USART3_IRQn);
    listen();
    NVIC_EnableIRQ(USART3_IRQn);
}

/* send reply, then resume listening. */
void rs485_send(const uint8_t *data, size_t len)
{
    if (len == 0u || len > sizeof s_tx) {
        rs485_release();
        return;
    }
    memcpy(s_tx, data, len);
    NVIC_DisableIRQ(USART3_IRQn);
    s_tx_len = len;
    s_tx_pos = 0;
    s_state = SENDING;
    driver(true);
    LL_USART_EnableIT_TXE_TXFNF(USART3);
    NVIC_EnableIRQ(USART3_IRQn);
}
