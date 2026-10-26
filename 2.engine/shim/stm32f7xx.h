/*
 * shim/stm32f7xx.h: found before st's own stm32f7xx.h on the include path.
 *
 * 1. st's real device header (types, bit constants, IRQn, *_BASE addresses);
 *    on wasm it pulls the generated core_cm7.h from shim-wasm/, on arm the real one.
 * 2. lxr_remap.h: every peripheral instance (GPIOA, USART3, TIM2, ...) points at a
 *    plain c object instead of its bus address.
 * 3. lxr_board.h: the virtual board interface (delay hooks replacing delay.h's
 *    busy waits, which the build skips with -DDWT_STM32_DELAY_H).
 * 4. st's real stm32f7xx.h (the device header guard is already set, so only its
 *    bit macros and the real stm32f7xx_hal.h follow).
 * no firmware file is changed; they all reach this through their own includes.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LXR_SHIM_STM32F7XX_H
#define LXR_SHIM_STM32F7XX_H
#include <stm32f765xx.h>
#include "lxr_remap.h"
#include "lxr_board.h"
#include_next <stm32f7xx.h>
#endif
