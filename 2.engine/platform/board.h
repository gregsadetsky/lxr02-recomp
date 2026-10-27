/*
 * board.h: virtual lxr-02 board, platform-internal interface.
 * the firmware never includes this; it sees only st's headers and lxr_board.h.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LXR_PLATFORM_BOARD_H
#define LXR_PLATFORM_BOARD_H
#include <stdint.h>
#include <stdio.h>
#include "stm32f7xx_hal.h"

#define LXR_HCLK 216000000u

/* ---- cost model (virtual cpu cycles). these set how much firmware work
 * happens between interrupts; they are model parameters, the same on every
 * build, so wasm and the cortex-m7 reference see the same interleaving. */
#define LXR_COST_HAL_CALL    1000u   /* every shimmed hal function */
#define LXR_COST_POLL          50u   /* DWT_check_nonblock spin */
#define LXR_COST_RENDER    160000u   /* one 64-frame block (~51% of the 1.45 ms budget) */

/* ---- time ---- */
extern uint64_t lxr_now;                 /* virtual cycles since reset */
void lxr_advance(uint64_t cycles);       /* advance time, dispatching due interrupts */
void lxr_board_init(void);

/* ---- hal <-> board ---- */
void lxr_board_i2s_started(I2S_HandleTypeDef *h, const uint16_t *buf, uint16_t size);
void lxr_board_adc_started(uint16_t *buf, uint32_t len);
void lxr_board_spi_receive(uint8_t *data, uint16_t n);
void lxr_board_spi_transmit(const uint8_t *data, uint16_t n);
void lxr_board_timer_changed(TIM_TypeDef *tim);
void lxr_board_systick_started(void);
uint32_t lxr_board_rng(void);
int lxr_board_sd_read(uint8_t *buf, uint32_t block, uint32_t n);
int lxr_board_sd_write(const uint8_t *buf, uint32_t block, uint32_t n);
uint32_t lxr_board_sd_blocks(void);

/* ---- scenario: scenario.txt in the run directory, loaded by the board ---- */
typedef enum {
    EV_UART_BYTES,   /* bytes[] arrive on the din midi input */
    EV_BUTTON,       /* arg0 = button index 0..39 (shift register), arg1 = 1 down / 0 up */
    EV_ENCODER,      /* arg0 = detents (+/-) */
    EV_ENC_BUTTON,   /* arg1 = 1 down / 0 up */
    EV_ADC,          /* arg0 = adc_buf index, arg1 = raw 16-bit value */
    EV_END           /* stop the run, write outputs */
} lxr_event_kind;
typedef struct {
    uint32_t ms;            /* virtual time of the event */
    lxr_event_kind kind;
    int32_t arg0, arg1;
    uint8_t bytes[16];
    uint8_t nbytes;
} lxr_event;
typedef struct {
    const char *name;
    uint32_t rng_seed;
    const lxr_event *events;
    uint32_t nevents;
} lxr_scenario;

#endif
