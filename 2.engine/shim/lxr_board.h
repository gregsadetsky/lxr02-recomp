/*
 * lxr_board.h: interface between the unmodified firmware and the virtual board.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LXR_BOARD_H
#define LXR_BOARD_H
#include <stdint.h>

/* replacements for the inline functions of Core/Inc/delay.h (skipped with
 * -DDWT_STM32_DELAY_H). same signatures. on hardware they spin on DWT->CYCCNT;
 * here they advance virtual time and are where lcd pin strobes are sampled. */
uint32_t DWT_Delay_Init(void);
void DWT_Delay_us(volatile uint32_t microseconds);
void DWT_init_nonblock(volatile uint32_t microseconds);
uint8_t DWT_check_nonblock(void);

/* called from every shimmed hal function and delay: advances virtual time and
 * dispatches due interrupts (dma complete, tim14, systick, tim2, uart rx). */
void lxr_board_hook(void);

#endif
