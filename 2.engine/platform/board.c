/*
 * board.c: the virtual lxr-02 board under the unmodified firmware.
 *
 * models only hardware: time, interrupt sources, gpio pins (with the
 * write-only BSRR semantics), the hd44780 lcd on its pins, the shift-register
 * buttons/leds, the pot/fader adc dma buffer, the midi uart, the two i2s dacs
 * and the sd card. firmware code paths are never replaced: interrupts are
 * delivered by calling the firmware's own handlers (SysTick_Handler,
 * TIM8_TRG_COM_TIM14_IRQHandler, TIM2_IRQHandler, USART3_IRQHandler,
 * HAL_I2S_TxCpltCallback).
 *
 * virtual time advances only through calls the firmware makes into the hal
 * and delay functions, plus a fixed cost per rendered audio block. the result
 * is deterministic and identical on wasm and on the cortex-m7 reference.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "main.h"
#include "stm32f7xx_it.h"
#include "Soundengine/globals.h"
#include "Parameters.h"
extern uint8_t activeParameters[];   /* Core/Inc/menu/menu.h */
extern uint8_t midi_MidiChannels[];
extern uint8_t midiParser_txRxFilter;
extern uint8_t menu_activePage;

void USART3_IRQHandler(void);                      /* Core/Src/Soundengine/MIDI/Uart.c */
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *h); /* Core/Src/Soundengine/cs4344_cs5343.c */

uint64_t lxr_now;
#if defined(__EMSCRIPTEN__)
volatile uint32_t lxr_primask;   /* state behind the portable __get/__set_PRIMASK (shim-wasm) */
volatile uint32_t lxr_fpscr;
#endif

static const lxr_scenario *sc;
static uint32_t ev_next;              /* next scenario event index */
static int in_isr;                    /* no nested interrupts are modelled */

/* ------------------------------------------------------------------ output */
static FILE *f_dac1, *f_dac2, *f_lcd, *f_midi;
static uint32_t blocks_out;
static uint32_t n_renders, n_rx_delivered, n_rx_dropped, n_tim2, n_tim14, n_late;

static void open_outputs(void)
{
    f_dac1 = fopen("dac1.raw", "wb");
    f_dac2 = fopen("dac2.raw", "wb");
    f_lcd = fopen("lcd.txt", "wb");
    f_midi = fopen("midiout.txt", "wb");
    if (!f_dac1 || !f_dac2 || !f_lcd || !f_midi) { fprintf(stderr, "board: cannot open outputs in the run directory\n"); exit(2); }
}

/* --------------------------------------------------------------- scenario
 * the run directory holds scenario.txt (see scenario/README.md for the format), the sd
 * card as card.img (absent = no card) and receives every output. the firmware image is
 * the same for every scenario. */
static lxr_event sc_events[8192];
static lxr_scenario sc_loaded;
static uint32_t hexbyte_list(const char *s, uint8_t *out, uint32_t cap)
{
    uint32_t n = 0;
    while (*s && n < cap) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '\n' || *s == '#') break;
        char *e; unsigned long v = strtoul(s, &e, 16);
        if (e == s || v > 255) { fprintf(stderr, "board: bad midi byte in scenario: %s\n", s); exit(2); }
        out[n++] = (uint8_t)v; s = e;
    }
    return n;
}
static int sc_live;                    /* scenario line "live": input lines also come from live.in */
static int parse_event(char *e, lxr_event *ev)
{
    while (*e == ' ') e++;
    memset(ev, 0, sizeof *ev);
    if (!strncmp(e, "midi ", 5)) { ev->kind = EV_UART_BYTES; ev->nbytes = (uint8_t)hexbyte_list(e + 5, ev->bytes, sizeof ev->bytes); }
    else if (!strncmp(e, "button ", 7)) { ev->kind = EV_BUTTON; ev->arg0 = (int32_t)strtol(e + 7, &e, 10); ev->arg1 = (int32_t)strtol(e, 0, 10); }
    else if (!strncmp(e, "encoder ", 8)) { ev->kind = EV_ENCODER; ev->arg0 = (int32_t)strtol(e + 8, 0, 10); }
    else if (!strncmp(e, "encbutton ", 10)) { ev->kind = EV_ENC_BUTTON; ev->arg1 = (int32_t)strtol(e + 10, 0, 10); }
    else if (!strncmp(e, "adc ", 4)) { ev->kind = EV_ADC; ev->arg0 = (int32_t)strtol(e + 4, &e, 10); ev->arg1 = (int32_t)strtol(e, 0, 0); }
    else if (!strncmp(e, "end", 3)) ev->kind = EV_END;
    else return 0;
    return 1;
}
static void load_scenario(void)
{
    FILE *f = fopen("scenario.txt", "rb");
    if (!f) { fprintf(stderr, "board: no scenario.txt in the run directory\n"); exit(2); }
    char line[256]; uint32_t n = 0, ln = 0;
    sc_loaded.name = "scenario.txt"; sc_loaded.rng_seed = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '\n' || *s == '#') continue;
        if (!strncmp(s, "seed ", 5)) { sc_loaded.rng_seed = (uint32_t)strtoul(s + 5, 0, 16); continue; }
        if (!strncmp(s, "card ", 5)) continue;                       /* for the run script */
        if (!strncmp(s, "live", 4)) { sc_live = 1; continue; }
        char *e; unsigned long ms = strtoul(s, &e, 10);
        if (e == s || n == sizeof sc_events / sizeof sc_events[0]) { fprintf(stderr, "board: scenario line %lu: %s", (unsigned long)ln, line); exit(2); }
        lxr_event *ev = &sc_events[n];
        if (!parse_event(e, ev)) { fprintf(stderr, "board: scenario line %lu: %s", (unsigned long)ln, line); exit(2); }
        ev->ms = (uint32_t)ms;
        if (n && ev->ms < sc_events[n - 1].ms) { fprintf(stderr, "board: scenario line %lu goes back in time\n", (unsigned long)ln); exit(2); }
        n++;
    }
    fclose(f);
    if (!sc_live && (!n || sc_events[n - 1].kind != EV_END)) { fprintf(stderr, "board: scenario has no end event\n"); exit(2); }
    sc_loaded.events = sc_events; sc_loaded.nevents = n;
    sc = &sc_loaded;
}

static unsigned long now_ms(void) { return (unsigned long)(lxr_now / (LXR_HCLK / 1000u)); }

/* --------------------------------------------------------------------- lcd
 * hd44780 in 4-bit mode on GPIOE: D4..D7 = PE7..PE10, E = PE11, RS = PE12.
 * the controller latches a nibble on the falling edge of E. */
static uint8_t lcd_ddram[128], lcd_cgram[64];
static uint8_t lcd_addr, lcd_cg_mode, lcd_mode8 = 1, lcd_have_hi, lcd_hi, lcd_inc = 1;
static uint32_t lcd_last_pins;
static int lcd_dirty;

static void lcd_dump(void)
{
    char l1[17], l2[17];
    for (int i = 0; i < 16; i++) {
        uint8_t a = lcd_ddram[i], b = lcd_ddram[0x40 + i];
        l1[i] = (a >= 32 && a < 127) ? (char)a : (a < 8 ? '#' : '?');
        l2[i] = (b >= 32 && b < 127) ? (char)b : (b < 8 ? '#' : '?');
    }
    l1[16] = l2[16] = 0;
    fprintf(f_lcd, "%8lu ms |%s|%s|\n", now_ms(), l1, l2);
}

static void lcd_byte(uint8_t rs, uint8_t v)
{
    if (rs) {
        if (lcd_cg_mode) lcd_cgram[lcd_addr & 63] = v; else lcd_ddram[lcd_addr & 127] = v;
        lcd_addr = (uint8_t)(lcd_addr + (lcd_inc ? 1 : -1));
        lcd_dirty = 1;
        return;
    }
    if (v & 0x80) { lcd_cg_mode = 0; lcd_addr = v & 0x7F; }
    else if (v & 0x40) { lcd_cg_mode = 1; lcd_addr = v & 0x3F; }
    else if (v & 0x20) { if (!(v & 0x10)) lcd_mode8 = 0; }        /* function set */
    else if (v & 0x04 && !(v & 0x08) && !(v & 0x10)) { lcd_inc = (v >> 1) & 1; }   /* entry mode */
    else if (v == 0x01) { memset(lcd_ddram, ' ', sizeof lcd_ddram); lcd_addr = 0; lcd_cg_mode = 0; lcd_dirty = 1; }
    else if ((v & 0xFE) == 0x02) { lcd_addr = 0; lcd_cg_mode = 0; }
}

static void lcd_pins(uint32_t pins)
{
    const uint32_t E = GPIO_PIN_11;
    if ((lcd_last_pins & E) && !(pins & E)) {          /* falling edge: latch */
        const uint8_t nib = (uint8_t)((lcd_last_pins >> 7) & 0xF);
        const uint8_t rs = (lcd_last_pins & GPIO_PIN_12) ? 1 : 0;
        if (lcd_mode8) {
            lcd_byte(rs, (uint8_t)(nib << 4));
        } else if (!lcd_have_hi) {
            lcd_hi = nib; lcd_have_hi = 1;
        } else {
            lcd_have_hi = 0;
            lcd_byte(rs, (uint8_t)((lcd_hi << 4) | nib));
        }
    }
    lcd_last_pins = pins;
}

/* --------------------------------------------------------------------- gpio */
GPIO_TypeDef *lxr_gpio_access(GPIO_TypeDef *p)
{
    const uint32_t bsrr = p->BSRR;
    if (bsrr) {
        /* set has priority over reset for the same pin (rm0410 gpio bsrr) */
        p->ODR = (p->ODR & ~(bsrr >> 16)) | (bsrr & 0xFFFFu);
        p->BSRR = 0;
    }
    if (p == &lxr_periph_GPIOE_BASE) lcd_pins(p->ODR);
    return p;
}

static void gpio_fold_all(void)
{
    (void)lxr_gpio_access(&lxr_periph_GPIOA_BASE); (void)lxr_gpio_access(&lxr_periph_GPIOB_BASE);
    (void)lxr_gpio_access(&lxr_periph_GPIOC_BASE); (void)lxr_gpio_access(&lxr_periph_GPIOD_BASE);
    (void)lxr_gpio_access(&lxr_periph_GPIOE_BASE);
}

/* ---------------------------------------------------------- shift registers */
static uint8_t buttons[5];      /* 40 inputs, 1 = pressed */
static uint8_t leds[5], leds_last[5], pd_last;

void lxr_board_spi_receive(uint8_t *data, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) data[i] = i < 5 ? buttons[i] : 0;
}
void lxr_board_spi_transmit(const uint8_t *data, uint16_t n)
{
    for (uint16_t i = 0; i < n && i < 5; i++) leds[i] = data[i];
}

/* ---------------------------------------------------------------------- adc */
static uint16_t *adc_dma; static uint32_t adc_len;
static uint16_t adc_values[16];
void lxr_board_adc_started(uint16_t *buf, uint32_t len)
{
    adc_dma = buf; adc_len = len;
    for (uint32_t i = 0; i < len && i < 16; i++) adc_dma[i] = adc_values[i];
}

/* ---------------------------------------------------------------------- rng */
static uint32_t rng_state;
uint32_t lxr_board_rng(void)
{   /* xorshift32. the device has a true rng; this is the one documented divergence. */
    uint32_t x = rng_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rng_state = x;
}

/* ------------------------------------------------------------------ sd card */
static FILE *card; static uint32_t card_blocks;
int lxr_board_sd_read(uint8_t *buf, uint32_t block, uint32_t n)
{
    if (!card || block + n > card_blocks) return -1;
    if (fseek(card, (long)block * 512L, SEEK_SET)) return -1;
    return fread(buf, 512, n, card) == n ? 0 : -1;
}
int lxr_board_sd_write(const uint8_t *buf, uint32_t block, uint32_t n)
{
    if (!card || block + n > card_blocks) return -1;
    if (fseek(card, (long)block * 512L, SEEK_SET)) return -1;
    return fwrite(buf, 512, n, card) == n ? 0 : -1;
}
uint32_t lxr_board_sd_blocks(void) { return card_blocks; }

/* ------------------------------------------------------------------- timers
 * apb1 timers (TIM2, TIM14) run at 108 MHz = hclk/2: one timer tick is
 * 2*(PSC+1) cpu cycles. update event when the counter passes ARR. TIM2 is 32
 * bit; a counter set above ARR (soundengine_process parks it at 1950 > 1500)
 * runs to the top and wraps, which takes minutes: no event in practice. */
typedef struct { TIM_TypeDef *t; uint64_t base_cycle; uint32_t base_cnt; uint32_t last_pub; int bits32; uint64_t due; int pending; } vtimer;
static vtimer vt2, vt14;

static uint64_t timer_due(vtimer *v)
{
    TIM_TypeDef *t = v->t;
    if (!(t->CR1 & TIM_CR1_CEN)) return UINT64_MAX;
    const uint64_t tick = 2ull * (t->PSC + 1u);
    const uint32_t arr = t->ARR;
    if (v->base_cnt > arr) return UINT64_MAX;
    return v->base_cycle + (uint64_t)(arr + 1u - v->base_cnt) * tick;
}
static void timer_sync(vtimer *v)
{   /* pick up a counter written by the firmware since the last sync */
    if (v->t->CNT != v->last_pub) { v->base_cnt = v->t->CNT; v->base_cycle = lxr_now; }
    v->due = timer_due(v);
}
static void timer_publish(vtimer *v)
{
    TIM_TypeDef *t = v->t;
    uint32_t cnt = v->base_cnt;
    if (t->CR1 & TIM_CR1_CEN) {
        const uint64_t tick = 2ull * (t->PSC + 1u);
        cnt = v->base_cnt + (uint32_t)((lxr_now - v->base_cycle) / tick);
    }
    t->CNT = v->bits32 ? cnt : (cnt & 0xFFFFu);
    v->last_pub = t->CNT;
}
void lxr_board_timer_changed(TIM_TypeDef *tim)
{
    vtimer *v = tim == &lxr_periph_TIM2_BASE ? &vt2 : tim == &lxr_periph_TIM14_BASE ? &vt14 : NULL;
    if (!v) return;
    v->base_cnt = tim->CNT; v->base_cycle = lxr_now; v->last_pub = tim->CNT;
    v->due = timer_due(v);
}

/* ----------------------------------------------------------------- systick */
static int systick_on; static uint64_t systick_due;
void lxr_board_systick_started(void) { systick_on = 1; systick_due = lxr_now + LXR_HCLK / 1000u; }

/* ---------------------------------------------------------------- i2s dacs
 * cs4344 dacs at fs = 22.6 MHz / 512 = 44140.625 Hz (plli2s n=113, r=5).
 * both streams transfer 64 stereo frames per dma round. */
#define I2S_FS_NUM 706250ull   /* fs = 706250/16 */
static const uint16_t *i2s_buf[2]; static uint16_t i2s_size[2];
static uint16_t i2s_snap[2][256];
static uint64_t dma_rounds;
/* both i2s streams run off one plli2s clock from the first HAL_I2S_Transmit_DMA on;
 * round n completes 64 frames after round n-1, counted from that start, not from reset. */
static uint64_t dma_t0 = UINT64_MAX;
static uint64_t dma_due_cycle(uint64_t round) { return dma_t0 + ((round + 1u) * 64u * (uint64_t)LXR_HCLK * 16u) / I2S_FS_NUM; }

void lxr_board_i2s_started(I2S_HandleTypeDef *h, const uint16_t *buf, uint16_t size)
{
    const int k = h->Instance == SPI2 ? 0 : 1;
    if (dma_t0 == UINT64_MAX) dma_t0 = lxr_now;
    i2s_buf[k] = buf; i2s_size[k] = size;
    /* the dma reads the buffer from the start of the round on (the first sample at once,
     * so the 32756 that CodecInit puts in dma_buffer[0] is heard). take the round's samples
     * at its start. in steady state the firmware only writes the other half, so this equals
     * a progressive read; only round 0, which the firmware renders into while it plays, is
     * a race on hardware that this does not model. */
    if (size > 256) { fprintf(stderr, "board: i2s round of %u samples > 256\n", size); exit(2); }
    memcpy(i2s_snap[k], buf, (size_t)size * 2u);
}

/* -------------------------------------------------------------------- uart
 * din midi on USART3, 31250 baud: one byte every 320 us. */
#define UART_BYTE_CYCLES (LXR_HCLK / 3125u)
static uint8_t rxq[4096]; static uint32_t rxq_head, rxq_tail; static uint64_t rx_due = UINT64_MAX;
static uint64_t tx_due = UINT64_MAX;

static void uart_rx_event(void)
{
    USART_TypeDef *u = USART3;
    if (rxq_head == rxq_tail) { rx_due = UINT64_MAX; return; }
    if (u->ISR & USART_ISR_RXNE) u->ISR |= USART_ISR_ORE;        /* previous byte never read */
    u->RDR = rxq[rxq_tail++ & 4095u];
    u->ISR |= USART_ISR_RXNE;
    if (u->CR1 & USART_CR1_RXNEIE) { in_isr = 1; USART3_IRQHandler(); in_isr = 0; n_rx_delivered++; } else n_rx_dropped++;
    u->ISR &= ~USART_ISR_RXNE;                                    /* the handler read RDR */
    rx_due = rxq_head != rxq_tail ? lxr_now + UART_BYTE_CYCLES : UINT64_MAX;
}
static void uart_tx_event(void)
{
    USART_TypeDef *u = USART3;
    if (!(u->CR1 & USART_CR1_TXEIE)) { tx_due = UINT64_MAX; return; }
    u->TDR = 0xFFFFu;                                             /* sentinel: write detection */
    u->ISR |= USART_ISR_TXE;
    in_isr = 1; USART3_IRQHandler(); in_isr = 0;
    if (u->TDR != 0xFFFFu) {
        fprintf(f_midi, "%8lu ms %02x\n", now_ms(), (unsigned)(u->TDR & 0xFF));
        u->ISR &= ~USART_ISR_TXE;
        tx_due = lxr_now + UART_BYTE_CYCLES;
    } else {
        tx_due = (u->CR1 & USART_CR1_TXEIE) ? lxr_now + UART_BYTE_CYCLES : UINT64_MAX;
    }
}

/* ------------------------------------------------------------------- encoder */
static int enc_pending;           /* detents still to turn (+/-) */
static int enc_phase;             /* 0..3 in the gray sequence 00 01 11 10 */
static uint64_t enc_due = UINT64_MAX;
static void enc_set_pins(void)
{
    static const uint8_t gray[4] = { 0, 1, 3, 2 };   /* A<<1 | B, forward = 00->01->11->10 */
    const uint8_t s = gray[enc_phase & 3];
    GPIO_TypeDef *e = &lxr_periph_GPIOE_BASE;
    e->IDR = (e->IDR & ~(uint32_t)(GPIO_PIN_13 | GPIO_PIN_14)) | ((s & 2) ? GPIO_PIN_13 : 0) | ((s & 1) ? GPIO_PIN_14 : 0);
}
static void enc_event(void)
{   /* one quarter step every 2 ms, sampled by the 1 ms SysTick */
    if (enc_pending == 0) { enc_due = UINT64_MAX; return; }
    enc_phase += enc_pending > 0 ? 1 : -1;
    if ((enc_phase & 3) == 0) enc_pending += enc_pending > 0 ? -1 : 1;
    enc_set_pins();
    enc_due = enc_pending ? lxr_now + 2u * (LXR_HCLK / 1000u) : UINT64_MAX;
}

/* ------------------------------------------------------------------ the end */
static void board_finish(void)
{
    gpio_fold_all();
    if (lcd_dirty) { lcd_dump(); lcd_dirty = 0; }
    fprintf(f_lcd, "end: %lu dma rounds, %lu blocks written, %lu renders, %lu late, tim2 %lu, tim14 %lu, uart rx %lu delivered %lu dropped\n",
            (unsigned long)dma_rounds, (unsigned long)blocks_out, (unsigned long)n_renders, (unsigned long)n_late, (unsigned long)n_tim2,
            (unsigned long)n_tim14, (unsigned long)n_rx_delivered, (unsigned long)n_rx_dropped);
    printf("board: voice volumes (activeParameters PAR_VOL1..6): %u %u %u %u %u %u\n", activeParameters[PAR_VOL1], activeParameters[PAR_VOL2],
           activeParameters[PAR_VOL3], activeParameters[PAR_VOL4], activeParameters[PAR_VOL5], activeParameters[PAR_VOL6]);
    printf("board: midi channels %u %u %u %u %u %u %u global %u, txrx filter 0x%02x, menu page %u\n", midi_MidiChannels[0], midi_MidiChannels[1], midi_MidiChannels[2],
           midi_MidiChannels[3], midi_MidiChannels[4], midi_MidiChannels[5], midi_MidiChannels[6], midi_MidiChannels[7], midiParser_txRxFilter, menu_activePage);
    printf("board: midi note filter (PAR_MIDI_NOTE1..7): %u %u %u %u %u %u %u\n", activeParameters[PAR_MIDI_NOTE1], activeParameters[PAR_MIDI_NOTE1+1],
           activeParameters[PAR_MIDI_NOTE1+2], activeParameters[PAR_MIDI_NOTE1+3], activeParameters[PAR_MIDI_NOTE1+4], activeParameters[PAR_MIDI_NOTE1+5], activeParameters[PAR_MIDI_NOTE1+6]);
    printf("board: renders %u, late %u, tim2 %u, tim14 %u, rx %u/%u\n", n_renders, n_late, n_tim2, n_tim14, n_rx_delivered, n_rx_dropped);
    {   /* the firmware's edit buffer at the end of the run, to check kit loads byte for byte */
        FILE *fp = fopen("params.txt", "w");
        for (int i = 0; i < NUM_PARAMS; i++) fprintf(fp, "%02x%s", activeParameters[i], (i % 32 == 31) ? "\n" : " ");
        fprintf(fp, "\n"); fclose(fp);
    }
    fclose(f_dac1); fclose(f_dac2); fclose(f_lcd); fclose(f_midi);
    if (card) fclose(card);
    printf("board: %s done at %u ms, %u blocks\n", sc->name, now_ms(), blocks_out);
    fflush(stdout);
    exit(0);
}

/* ---------------------------------------------------------- scenario events */
static uint64_t ev_due(void)
{
    if (ev_next >= sc->nevents) return UINT64_MAX;
    return (uint64_t)sc->events[ev_next].ms * (LXR_HCLK / 1000u);
}
static void apply_event(const lxr_event *e);
static void scenario_event(void) { apply_event(&sc->events[ev_next++]); }
static void apply_event(const lxr_event *e)
{
    switch (e->kind) {
    case EV_UART_BYTES:
        for (int i = 0; i < e->nbytes; i++) rxq[rxq_head++ & 4095u] = e->bytes[i];
        if (rx_due == UINT64_MAX) rx_due = lxr_now;
        break;
    case EV_BUTTON:
        if (e->arg0 >= 0 && e->arg0 < 40) {
            if (e->arg1) buttons[e->arg0 / 8] |= (uint8_t)(1u << (e->arg0 % 8));
            else buttons[e->arg0 / 8] &= (uint8_t)~(1u << (e->arg0 % 8));
        } else if (e->arg0 == 40) {   /* BUT_PAGE_DOWN: lone switch on SW_43 (PB7), high = pressed (DIO.c) */
            if (e->arg1) lxr_periph_GPIOB_BASE.IDR |= GPIO_PIN_7; else lxr_periph_GPIOB_BASE.IDR &= ~(uint32_t)GPIO_PIN_7;
        }
        break;
    case EV_ENCODER:
        enc_pending += e->arg0;
        if (enc_due == UINT64_MAX) enc_due = lxr_now;
        break;
    case EV_ENC_BUTTON:   /* pulled up: 1 = released */
        if (e->arg1) lxr_periph_GPIOE_BASE.IDR &= ~(uint32_t)GPIO_PIN_15; else lxr_periph_GPIOE_BASE.IDR |= GPIO_PIN_15;
        break;
    case EV_ADC:
        if (e->arg0 >= 0 && e->arg0 < 16) { adc_values[e->arg0] = (uint16_t)e->arg1; if (adc_dma && (uint32_t)e->arg0 < adc_len) adc_dma[e->arg0] = (uint16_t)e->arg1; }
        break;
    case EV_END:
        board_finish();
        break;
    }
}

/* ------------------------------------------------------------- live input
 * live mode: once per dma round, read whatever input lines the host has (live.in, the
 * scenario line format without a time) and apply them now. a plain file works too
 * (everything applies at the first round); the browser host feeds it from web midi and
 * the panel. reads go straight to the semihosting channel (no stdio buffering). */
#include <fcntl.h>
#include <unistd.h>
/* a timed live line joins the scenario queue (kept in time order), so it fires at its exact
 * virtual millisecond like a scenario.txt event; one already due fires now. */
static void live_schedule(const lxr_event *ev)
{
    if ((uint64_t)ev->ms * (LXR_HCLK / 1000u) <= lxr_now) { apply_event(ev); return; }
    if (sc_loaded.nevents == sizeof sc_events / sizeof sc_events[0]) { fprintf(stderr, "board: live queue full\n"); return; }
    uint32_t k = sc_loaded.nevents;
    while (k > ev_next && sc_events[k - 1].ms > ev->ms) { sc_events[k] = sc_events[k - 1]; k--; }
    sc_events[k] = *ev; sc_loaded.nevents++;
}
static int live_fd = -1; static char live_buf[1024]; static unsigned live_len;
static void live_poll(void)
{
    if (live_fd < 0) { live_fd = open("live.in", O_RDONLY); if (live_fd < 0) { fprintf(stderr, "board: live mode without live.in\n"); exit(2); } }
    int r = (int)read(live_fd, live_buf + live_len, sizeof live_buf - 1 - live_len);
    if (r <= 0) return;
    live_len += (unsigned)r;
    char *p = live_buf, *nl;
    while ((nl = memchr(p, '\n', live_len - (unsigned)(p - live_buf)))) {
        *nl = 0; lxr_event ev;
        if (*p && *p != '#') {
            char *e = p; unsigned long ms = strtoul(p, &e, 10);
            if (!parse_event(e, &ev)) fprintf(stderr, "board: bad live line: %s\n", p);
            else if (e == p) apply_event(&ev);                       /* no time: now */
            else { ev.ms = (uint32_t)ms; live_schedule(&ev); }       /* timed: into the scenario queue */
        }
        p = nl + 1;
    }
    live_len -= (unsigned)(p - live_buf); memmove(live_buf, p, live_len);
    if (live_len == sizeof live_buf - 1) live_len = 0;   /* a line longer than the buffer: drop */
}

/* -------------------------------------------------------------- dispatcher */
static void dma_event(void)
{
    if (bCurrentSampleValid != SAMPLE_VALID) {   /* the firmware counts this as error++ */
        if (n_late < 16) printf("board: late block, dma round %lu at %u ms\n", (unsigned long)dma_rounds, now_ms());
        n_late++;
    }
    if (sc_live) live_poll();
    /* the round that just finished played what the dma was pointed at */
    if (i2s_buf[0]) fwrite(i2s_snap[0], 2, i2s_size[0], f_dac1);
    if (i2s_buf[1]) fwrite(i2s_snap[1], 2, i2s_size[1], f_dac2);
    if (i2s_buf[0] || i2s_buf[1]) blocks_out++;
    dma_rounds++;
    /* transfer complete: SPI2 then SPI3, through the firmware's own callback */
    in_isr = 1;
    if (i2s_buf[0]) HAL_I2S_TxCpltCallback(&hi2s2);
    if (i2s_buf[1]) HAL_I2S_TxCpltCallback(&hi2s3);
    in_isr = 0;
}

enum { SRC_TIM14, SRC_SYSTICK, SRC_DMA, SRC_TIM2, SRC_UART_RX, SRC_UART_TX, SRC_ENC, SRC_SCENARIO, SRC_N };

static uint64_t due_of(int s)
{
    switch (s) {
    case SRC_TIM14: return (vt14.t->DIER & TIM_DIER_UIE) ? vt14.due : UINT64_MAX;
    case SRC_SYSTICK: return systick_on ? systick_due : UINT64_MAX;
    case SRC_DMA: return (i2s_buf[0] || i2s_buf[1]) ? dma_due_cycle(dma_rounds) : UINT64_MAX;
    case SRC_TIM2: return (vt2.t->DIER & TIM_DIER_UIE) ? vt2.due : UINT64_MAX;
    case SRC_UART_RX: return rx_due;
    case SRC_UART_TX: return (USART3->CR1 & USART_CR1_TXEIE) ? (tx_due == UINT64_MAX ? lxr_now : tx_due) : UINT64_MAX;
    case SRC_ENC: return enc_due;
    case SRC_SCENARIO: return ev_due();
    }
    return UINT64_MAX;
}

static void fire(int s)
{
    switch (s) {
    case SRC_TIM14:
        vt14.base_cnt = 0; vt14.base_cycle = vt14.due; vt14.due = timer_due(&vt14);
        timer_publish(&vt14);
        in_isr = 1; TIM8_TRG_COM_TIM14_IRQHandler(); in_isr = 0; n_tim14++;
        break;
    case SRC_SYSTICK:
        systick_due += LXR_HCLK / 1000u;
        in_isr = 1; SysTick_Handler(); in_isr = 0;
        break;
    case SRC_DMA: dma_event(); break;
    case SRC_TIM2:
        vt2.base_cnt = 0; vt2.base_cycle = vt2.due; vt2.due = timer_due(&vt2);
        timer_publish(&vt2);
        in_isr = 1; TIM2_IRQHandler(); in_isr = 0; n_tim2++;
        break;
    case SRC_UART_RX: uart_rx_event(); break;
    case SRC_UART_TX: uart_tx_event(); break;
    case SRC_ENC: enc_event(); break;
    case SRC_SCENARIO: scenario_event(); break;
    }
    timer_sync(&vt2); timer_sync(&vt14);
}

static uint8_t render_flag_last;

void lxr_advance(uint64_t cycles)
{
    /* a block rendered since the last call costs cpu time too */
    if (bCurrentSampleValid == SAMPLE_VALID && render_flag_last != SAMPLE_VALID) { cycles += LXR_COST_RENDER; n_renders++; }
    render_flag_last = bCurrentSampleValid;

    const uint64_t target = lxr_now + cycles;
    timer_sync(&vt2); timer_sync(&vt14);
    gpio_fold_all();
    if (in_isr || __get_PRIMASK()) {   /* real PRIMASK on arm, the shim's on wasm */ lxr_now = target; timer_publish(&vt2); timer_publish(&vt14); return; }
    for (;;) {
        int best = -1; uint64_t bd = UINT64_MAX;
        for (int s = 0; s < SRC_N; s++) { const uint64_t d = due_of(s); if (d < bd) { bd = d; best = s; } }
        if (best < 0 || bd > target) break;
        if (bd > lxr_now) lxr_now = bd;
        timer_publish(&vt2); timer_publish(&vt14);
        fire(best);
        render_flag_last = bCurrentSampleValid;
        if (lcd_dirty) { lcd_dump(); lcd_dirty = 0; }
    }
    lxr_now = target;
    timer_publish(&vt2); timer_publish(&vt14);
    /* LED_PAGE_DOWN is not in the shift register: LED_43 on PB8 (ledHandler.c) */
    const uint8_t pd = (lxr_periph_GPIOB_BASE.ODR & GPIO_PIN_8) ? 1 : 0;
    if (memcmp(leds, leds_last, 5) || pd != pd_last) {
        memcpy(leds_last, leds, 5); pd_last = pd;
        fprintf(f_lcd, "%8lu ms leds %02x %02x %02x %02x %02x %02x\n", now_ms(), leds[0], leds[1], leds[2], leds[3], leds[4], pd);
    }
}

void lxr_board_hook(void) { lxr_advance(LXR_COST_HAL_CALL); }

/* --------------------------------------------- delay.h replacements (dwt) */
static uint64_t nb_target;
uint32_t DWT_Delay_Init(void) { return 0; }      /* the cycle counter always runs here */
void DWT_Delay_us(volatile uint32_t us) { lxr_advance((uint64_t)us * (LXR_HCLK / 1000000u)); }
void DWT_init_nonblock(volatile uint32_t us) { lxr_advance(0); nb_target = lxr_now + (uint64_t)us * (LXR_HCLK / 1000000u); }
uint8_t DWT_check_nonblock(void) { lxr_advance(LXR_COST_POLL); return lxr_now >= nb_target; }

/* ----------------------------------------------------------------- startup */
void lxr_board_init(void)
{
    load_scenario();
    open_outputs();
    if (sc_live) { setvbuf(f_dac1, 0, _IONBF, 0); setvbuf(f_dac2, 0, _IONBF, 0); setvbuf(f_lcd, 0, _IONBF, 0); setvbuf(f_midi, 0, _IONBF, 0); }
    rng_state = sc->rng_seed ? sc->rng_seed : 0x12345678u;
    card = fopen("card.img", "r+b");
    if (card) { fseek(card, 0, SEEK_END); card_blocks = (uint32_t)(ftell(card) / 512); fseek(card, 0, SEEK_SET); }
    vt2.t = TIM2; vt2.bits32 = 1; vt14.t = TIM14; vt14.bits32 = 0;
    /* input pins at rest. jack detect: 1 = cable in (all outputs plugged).
     * sd detect PD1: 0 = card present. encoder switch PE15 pulled up = released.
     * SW_43 PB7: 0 = released. */
    lxr_periph_GPIOB_BASE.IDR = GPIO_PIN_4 | GPIO_PIN_6;
    lxr_periph_GPIOD_BASE.IDR = GPIO_PIN_6 | GPIO_PIN_7 | (card ? 0u : GPIO_PIN_1);
    lxr_periph_GPIOE_BASE.IDR = GPIO_PIN_15;
    enc_set_pins();
    /* pots: both wipers mid; faders 8..13 fully up */
    for (int i = 0; i < 8; i++) adc_values[i] = 0x8000u;
    for (int i = 8; i < 14; i++) adc_values[i] = 0xFFF0u;
    memset(lcd_ddram, ' ', sizeof lcd_ddram);
    printf("board: %lu scenario events, card %s (%lu blocks)\n", (unsigned long)sc->nevents, card ? "card.img" : "none", (unsigned long)card_blocks);
}
