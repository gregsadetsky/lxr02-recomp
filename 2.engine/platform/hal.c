/*
 * hal.c: bodies of the st hal functions the firmware calls, on the virtual
 * board. declarations, types and constants are st's own headers, unchanged.
 * these replace st's driver .c files only (the hardware layer); every
 * firmware callback and msp init is still the firmware's own code.
 * every function advances virtual time (lxr_board_hook), which is where
 * interrupts get delivered, as on hardware where they arrive while the
 * firmware is busy talking to peripherals.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <string.h>
#include "board.h"

#define HOOK() lxr_board_hook()

/* ------------------------------------------------------------ core / clocks */
__IO uint32_t uwTick;
uint32_t uwTickPrio = (1UL << __NVIC_PRIO_BITS);
HAL_TickFreqTypeDef uwTickFreq = HAL_TICK_FREQ_DEFAULT;

__attribute__((constructor)) static void lxr_ctor(void) { lxr_board_init(); }

HAL_StatusTypeDef HAL_Init(void) { lxr_board_systick_started(); uwTickPrio = TICK_INT_PRIORITY; HAL_MspInit(); HOOK(); return HAL_OK; }
void HAL_IncTick(void) { uwTick += uwTickFreq; }
uint32_t HAL_GetTick(void) { HOOK(); return uwTick; }
void HAL_Delay(uint32_t Delay)
{   /* as st's: waits at least Delay ms, counting SysTick increments */
    const uint32_t start = uwTick;
    uint32_t wait = Delay;
    if (wait < HAL_MAX_DELAY) wait += (uint32_t)uwTickFreq;
    while ((uwTick - start) < wait) lxr_advance(LXR_HCLK / 10000u);
}
HAL_StatusTypeDef HAL_RCC_OscConfig(RCC_OscInitTypeDef *p) { (void)p; HOOK(); return HAL_OK; }
HAL_StatusTypeDef HAL_RCC_ClockConfig(RCC_ClkInitTypeDef *p, uint32_t l) { (void)p; (void)l; SystemCoreClock = LXR_HCLK; HOOK(); return HAL_OK; }
HAL_StatusTypeDef HAL_RCCEx_PeriphCLKConfig(RCC_PeriphCLKInitTypeDef *p) { (void)p; HOOK(); return HAL_OK; }
HAL_StatusTypeDef HAL_PWREx_EnableOverDrive(void) { HOOK(); return HAL_OK; }
uint32_t HAL_RCC_GetHCLKFreq(void) { return LXR_HCLK; }
uint32_t HAL_RCC_GetSysClockFreq(void) { return LXR_HCLK; }
uint32_t HAL_RCC_GetPCLK1Freq(void) { return LXR_HCLK / 4u; }
uint32_t HAL_RCC_GetPCLK2Freq(void) { return LXR_HCLK / 2u; }
void HAL_NVIC_SetPriority(IRQn_Type n, uint32_t p, uint32_t s) { (void)n; (void)p; (void)s; HOOK(); }
void HAL_NVIC_EnableIRQ(IRQn_Type n) { (void)n; HOOK(); }
void HAL_NVIC_SetPriorityGrouping(uint32_t g) { (void)g; HOOK(); }
void HAL_NVIC_DisableIRQ(IRQn_Type n) { (void)n; HOOK(); }

/* --------------------------------------------------------------------- gpio */
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init) { (void)lxr_gpio_access(port); (void)init; HOOK(); }
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    HOOK();
    return (lxr_gpio_access(port)->IDR & pin) ? GPIO_PIN_SET : GPIO_PIN_RESET;
}
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState s)
{   /* st writes BSRR; the port applies it at once */
    GPIO_TypeDef *p = lxr_gpio_access(port);
    p->BSRR = s != GPIO_PIN_RESET ? (uint32_t)pin : (uint32_t)pin << 16;
    (void)lxr_gpio_access(port);
    HOOK();
}
void HAL_GPIO_TogglePin(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_TypeDef *p = lxr_gpio_access(port);
    p->ODR ^= pin;
    (void)lxr_gpio_access(port);
    HOOK();
}

/* ---------------------------------------------------------------------- dma */
HAL_StatusTypeDef HAL_DMA_Init(DMA_HandleTypeDef *h) { h->State = HAL_DMA_STATE_READY; HOOK(); return HAL_OK; }
HAL_StatusTypeDef HAL_DMA_DeInit(DMA_HandleTypeDef *h) { h->State = HAL_DMA_STATE_RESET; HOOK(); return HAL_OK; }
void HAL_DMA_IRQHandler(DMA_HandleTypeDef *h) { (void)h; }   /* completions are delivered by the board */

/* ---------------------------------------------------------------------- adc */
HAL_StatusTypeDef HAL_ADC_Init(ADC_HandleTypeDef *h)
{
    if (h->State == HAL_ADC_STATE_RESET) HAL_ADC_MspInit(h);
    h->State = HAL_ADC_STATE_READY; HOOK(); return HAL_OK;
}
HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef *h, ADC_ChannelConfTypeDef *c) { (void)h; (void)c; HOOK(); return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_Start_DMA(ADC_HandleTypeDef *h, uint32_t *data, uint32_t len)
{   /* continuous conversion into a 16-bit buffer (the firmware passes a uint16_t array) */
    (void)h; lxr_board_adc_started((uint16_t *)data, len); HOOK(); return HAL_OK;
}

/* ---------------------------------------------------------------------- i2s */
HAL_StatusTypeDef HAL_I2S_Init(I2S_HandleTypeDef *h)
{
    if (h->State == HAL_I2S_STATE_RESET) { h->Lock = HAL_UNLOCKED; HAL_I2S_MspInit(h); }
    h->State = HAL_I2S_STATE_READY; HOOK(); return HAL_OK;
}
HAL_StatusTypeDef HAL_I2S_Transmit_DMA(I2S_HandleTypeDef *h, uint16_t *data, uint16_t size)
{
    h->pTxBuffPtr = data; h->TxXferSize = size; h->TxXferCount = size;
    lxr_board_i2s_started(h, data, size);
    HOOK(); return HAL_OK;
}

/* ---------------------------------------------------------------------- spi */
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *h)
{
    if (h->State == HAL_SPI_STATE_RESET) { h->Lock = HAL_UNLOCKED; HAL_SPI_MspInit(h); }
    h->State = HAL_SPI_STATE_READY; HOOK(); return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t t)
{   /* SPI1 reads the 74hc165 button chain */
    (void)h; (void)t; lxr_board_spi_receive(d, n); HOOK(); return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t t)
{   /* SPI1 writes the 74hc595 led chain */
    (void)h; (void)t; lxr_board_spi_transmit(d, n); HOOK(); return HAL_OK;
}

/* ---------------------------------------------------------------------- tim */
HAL_StatusTypeDef HAL_TIM_Base_Init(TIM_HandleTypeDef *h)
{
    if (h->State == HAL_TIM_STATE_RESET) { h->Lock = HAL_UNLOCKED; HAL_TIM_Base_MspInit(h); }
    h->Instance->PSC = h->Init.Prescaler;
    h->Instance->ARR = h->Init.Period;
    h->Instance->CNT = 0;
    h->State = HAL_TIM_STATE_READY;
    lxr_board_timer_changed(h->Instance);
    HOOK(); return HAL_OK;
}
HAL_StatusTypeDef HAL_TIM_OC_Init(TIM_HandleTypeDef *h) { return HAL_TIM_Base_Init(h); }
HAL_StatusTypeDef HAL_TIM_ConfigClockSource(TIM_HandleTypeDef *h, TIM_ClockConfigTypeDef *c) { (void)h; (void)c; HOOK(); return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_OC_ConfigChannel(TIM_HandleTypeDef *h, TIM_OC_InitTypeDef *c, uint32_t ch) { (void)h; (void)c; (void)ch; HOOK(); return HAL_OK; }
HAL_StatusTypeDef HAL_TIMEx_MasterConfigSynchronization(TIM_HandleTypeDef *h, TIM_MasterConfigTypeDef *c) { (void)h; (void)c; HOOK(); return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *h)
{
    lxr_board_timer_changed(h->Instance);
    h->Instance->DIER |= TIM_DIER_UIE;
    h->Instance->CR1 |= TIM_CR1_CEN;
    lxr_board_timer_changed(h->Instance);
    HOOK(); return HAL_OK;
}
HAL_StatusTypeDef HAL_TIM_Base_Stop_IT(TIM_HandleTypeDef *h)
{
    h->Instance->DIER &= ~TIM_DIER_UIE;
    h->Instance->CR1 &= ~TIM_CR1_CEN;
    lxr_board_timer_changed(h->Instance);
    HOOK(); return HAL_OK;
}
HAL_StatusTypeDef HAL_TIM_Base_Stop(TIM_HandleTypeDef *h)
{
    h->Instance->CR1 &= ~TIM_CR1_CEN;
    lxr_board_timer_changed(h->Instance);
    HOOK(); return HAL_OK;
}
void HAL_TIM_IRQHandler(TIM_HandleTypeDef *h) { h->Instance->SR &= ~TIM_SR_UIF; }

/* --------------------------------------------------------------------- uart */
HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *h)
{
    if (h->gState == HAL_UART_STATE_RESET) { h->Lock = HAL_UNLOCKED; HAL_UART_MspInit(h); }
    h->Instance->CR1 |= USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
    h->gState = HAL_UART_STATE_READY; h->RxState = HAL_UART_STATE_READY;
    HOOK(); return HAL_OK;
}

/* ---------------------------------------------------------------------- rng */
HAL_StatusTypeDef HAL_RNG_Init(RNG_HandleTypeDef *h)
{
    if (h->State == HAL_RNG_STATE_RESET) { h->Lock = HAL_UNLOCKED; HAL_RNG_MspInit(h); }
    h->State = HAL_RNG_STATE_READY; HOOK(); return HAL_OK;
}
uint32_t HAL_RNG_GetRandomNumber(RNG_HandleTypeDef *h) { (void)h; return lxr_board_rng(); }

/* ----------------------------------------------------------------------- sd */
HAL_StatusTypeDef HAL_SD_Init(SD_HandleTypeDef *h)
{
    if (h->State == HAL_SD_STATE_RESET) { h->Lock = HAL_UNLOCKED; HAL_SD_MspInit(h); }
    if (!lxr_board_sd_blocks()) { h->State = HAL_SD_STATE_RESET; HOOK(); return HAL_ERROR; }
    h->SdCard.CardType = CARD_SDHC_SDXC; h->SdCard.BlockNbr = lxr_board_sd_blocks(); h->SdCard.BlockSize = 512;
    h->SdCard.LogBlockNbr = lxr_board_sd_blocks(); h->SdCard.LogBlockSize = 512;
    h->State = HAL_SD_STATE_READY; HOOK(); return HAL_OK;
}
HAL_StatusTypeDef HAL_SD_ReadBlocks(SD_HandleTypeDef *h, uint8_t *d, uint32_t blk, uint32_t n, uint32_t t)
{
    (void)h; (void)t; const int r = lxr_board_sd_read(d, blk, n);
    lxr_advance(LXR_COST_HAL_CALL * (1u + n));   /* the card takes time per block */
    return r ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef HAL_SD_WriteBlocks(SD_HandleTypeDef *h, uint8_t *d, uint32_t blk, uint32_t n, uint32_t t)
{
    (void)h; (void)t; const int r = lxr_board_sd_write(d, blk, n);
    lxr_advance(LXR_COST_HAL_CALL * (1u + 4u * n));
    return r ? HAL_ERROR : HAL_OK;
}
HAL_SD_CardStateTypeDef HAL_SD_GetCardState(SD_HandleTypeDef *h) { (void)h; HOOK(); return HAL_SD_CARD_TRANSFER; }
HAL_StatusTypeDef HAL_SD_GetCardInfo(SD_HandleTypeDef *h, HAL_SD_CardInfoTypeDef *ci)
{
    ci->CardType = h->SdCard.CardType; ci->CardVersion = CARD_V2_X; ci->Class = 0; ci->RelCardAdd = 1;
    ci->BlockNbr = h->SdCard.BlockNbr; ci->BlockSize = h->SdCard.BlockSize;
    ci->LogBlockNbr = h->SdCard.LogBlockNbr; ci->LogBlockSize = h->SdCard.LogBlockSize;
    HOOK(); return HAL_OK;
}
void HAL_SD_IRQHandler(SD_HandleTypeDef *h) { (void)h; }

/* ---------------------------------------------------------------------- usb
 * no host is attached: the device stack initialises and never enumerates. */
HAL_StatusTypeDef HAL_PCD_Init(PCD_HandleTypeDef *h)
{
    if (h->State == HAL_PCD_STATE_RESET) { h->Lock = HAL_UNLOCKED; HAL_PCD_MspInit(h); }
    h->State = HAL_PCD_STATE_READY; HOOK(); return HAL_OK;
}
HAL_StatusTypeDef HAL_PCD_Start(PCD_HandleTypeDef *h) { (void)h; HOOK(); return HAL_OK; }
HAL_StatusTypeDef HAL_PCDEx_SetRxFiFo(PCD_HandleTypeDef *h, uint16_t s) { (void)h; (void)s; return HAL_OK; }
HAL_StatusTypeDef HAL_PCDEx_SetTxFiFo(PCD_HandleTypeDef *h, uint8_t f, uint16_t s) { (void)h; (void)f; (void)s; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_EP_Open(PCD_HandleTypeDef *h, uint8_t a, uint16_t m, uint8_t t) { (void)h; (void)a; (void)m; (void)t; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_EP_Close(PCD_HandleTypeDef *h, uint8_t a) { (void)h; (void)a; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_EP_Receive(PCD_HandleTypeDef *h, uint8_t a, uint8_t *b, uint32_t l) { (void)h; (void)a; (void)b; (void)l; return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_EP_Transmit(PCD_HandleTypeDef *h, uint8_t a, uint8_t *b, uint32_t l) { (void)h; (void)a; (void)b; (void)l; HOOK(); return HAL_OK; }
HAL_StatusTypeDef HAL_PCD_EP_SetStall(PCD_HandleTypeDef *h, uint8_t a) { (void)h; (void)a; return HAL_OK; }
uint32_t HAL_PCD_EP_GetRxCount(PCD_HandleTypeDef *h, uint8_t a) { (void)h; (void)a; return 0; }
void HAL_PCD_IRQHandler(PCD_HandleTypeDef *h) { (void)h; }
