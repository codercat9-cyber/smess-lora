/*
 * STORAGE&MESS LoRa на STM32 (libopencm3).
 *
 * Подключение (одинаково для Black Pill F411 и Blue Pill F103):
 *   USART2  PA2 (TX) → RX узла Meshtastic,  PA3 (RX) ← TX узла   — 115200
 *   USART1  PA9 (TX) → RX USB-UART адаптера, PA10 (RX) ← TX       — консоль, 115200
 *   PC13    светодиод: мигает — подключаюсь, горит — узел на связи
 *   Общая земля с узлом обязательна. Уровни 3,3 В — как у ESP32/nRF52.
 *
 * На узле: Serial-модуль не нужен — работает обычный Serial API на его
 * USB/UART-консоли. В прошивках 2.5+ проверьте Security → Serial console = вкл.
 *
 * Для модуля защиты (полная сборка) плата даёт шум АЦП как источник
 * случайности (аппаратного ГСЧ у F411 и F103 нет) и одну страницу флеша под
 * секрет: F103C8 — последний килобайт (0x0800FC00), F411 — сектор 7
 * (0x08060000). Чтобы секрет нельзя было считать программатором, после
 * прошивки включите защиту чтения: make protect.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/desig.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>

#include "app.h"

#define RING 1024 /* степень двойки */

typedef struct {
    volatile uint8_t buf[RING];
    volatile uint16_t head, tail;
} ring_t;

static ring_t radio_rx, console_rx;
static volatile uint32_t ms;

static void ring_put(ring_t *r, uint8_t b) {
    uint16_t next = (uint16_t)((r->head + 1) & (RING - 1));
    if (next != r->tail) { /* полон — байт теряется, но не портит очередь */
        r->buf[r->head] = b;
        r->head = next;
    }
}

static int ring_get(ring_t *r) {
    if (r->tail == r->head) return -1;
    uint8_t b = r->buf[r->tail];
    r->tail = (uint16_t)((r->tail + 1) & (RING - 1));
    return b;
}

void sys_tick_handler(void) {
    ms++;
}

/* Чтение SR, затем DR снимает и RXNE, и ошибки (ORE/NE/FE). DR читаем
 * ВСЕГДА: если прерывание взвелось без RXNE (ошибка линии, особенности
 * модели), без чтения оно не снимется и обработчик будет вызываться по
 * кругу, а основной цикл встанет — поймано в эмуляторе Renode. */
static void usart_rx(uint32_t usart, ring_t *ring) {
    uint32_t sr = USART_SR(usart);
    uint8_t b = (uint8_t)USART_DR(usart);
    if (sr & USART_SR_RXNE) ring_put(ring, b);
}

void usart2_isr(void) { usart_rx(USART2, &radio_rx); }
void usart1_isr(void) { usart_rx(USART1, &console_rx); }

static void radio_write(const uint8_t *d, size_t n) {
    while (n--) usart_send_blocking(USART2, *d++);
}

static void console_write(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') usart_send_blocking(USART1, '\r');
        usart_send_blocking(USART1, (uint8_t)*s);
    }
}

static uint32_t millis(void) {
    return ms;
}

/* Случайность без аппаратного ГСЧ (у F411 и F103 его нет): уникальный
 * номер кристалла + время + xorshift. Для id пакетов этого достаточно —
 * криптографическая случайность нужна только закрытому модулю, он берёт свою. */
static uint32_t rng_state;
static uint32_t random32(void) {
    uint32_t x = rng_state ^ ms;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* --- для модуля защиты: шум АЦП и страница флеша --- */

#if defined(STM32F4)
#define SECRET_ADDR 0x08060000u
#define ADC_NOISE_CH 18 /* датчик температуры F411 */
#else
#define SECRET_ADDR 0x0800FC00u
#define ADC_NOISE_CH ADC_CHANNEL_TEMP
#endif

static bool adc_ok;

static void adc_setup(void) {
    rcc_periph_clock_enable(RCC_ADC1);
    adc_power_off(ADC1);
#if defined(STM32F4)
    adc_set_clk_prescale(ADC_CCR_ADCPRE_BY4);
    adc_set_sample_time(ADC1, ADC_NOISE_CH, ADC_SMPR_SMP_3CYC);
#else
    rcc_set_adcpre(RCC_CFGR_ADCPRE_DIV6); /* 12 МГц: больше АЦП F103 не умеет */
    adc_set_sample_time(ADC1, ADC_NOISE_CH, ADC_SMPR_SMP_1DOT5CYC);
#endif
    adc_disable_scan_mode(ADC1);
    adc_set_single_conversion_mode(ADC1);
    adc_disable_external_trigger_regular(ADC1);
    adc_set_right_aligned(ADC1);
    adc_enable_temperature_sensor();
    uint8_t ch = ADC_NOISE_CH;
    adc_set_regular_sequence(ADC1, 1, &ch);
    adc_power_on(ADC1);
    for (volatile int i = 0; i < 1000; i++) { /* АЦП просыпается несколько мкс */
    }
    adc_ok = true;
}

/* Самое короткое время выборки — младшие биты почти чистый шум. */
static uint32_t adc_sample(void) {
    if (!adc_ok) return 0;
#if defined(STM32F4)
    adc_start_conversion_regular(ADC1);
#else
    adc_start_conversion_direct(ADC1);
#endif
    for (int t = 0; t < 2000; t++)
        if (adc_eoc(ADC1)) return adc_read_regular(ADC1);
    adc_ok = false; /* АЦП не отвечает (бывает в эмуляторе) — дальше без него */
    return 0;
}

static void entropy(uint8_t *buf, size_t n) {
    uint32_t uid[3];
    desig_get_unique_id(uid);
    for (size_t i = 0; i < n; i++) {
        uint32_t x = uid[i % 3] ^ STK_CVR ^ (ms << 16);
        for (int k = 0; k < 8; k++) x = (x << 5 | x >> 27) ^ adc_sample() ^ STK_CVR;
        buf[i] = (uint8_t)(x ^ x >> 8 ^ x >> 16 ^ x >> 24);
    }
}

static bool secret_load(uint8_t *buf, size_t n) {
    memcpy(buf, (const void *)SECRET_ADDR, n);
    return true; /* целостность проверит модуль: стёртый флеш — это FF */
}

static bool secret_store(const uint8_t *buf, size_t n) {
    flash_unlock();
#if defined(STM32F4)
    flash_erase_sector(7, FLASH_CR_PROGRAM_X32);
#else
    flash_erase_page(SECRET_ADDR);
#endif
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = 0xFFFFFFFFu;
        memcpy(&w, buf + i, n - i < 4 ? n - i : 4);
        flash_program_word(SECRET_ADDR + i, w);
    }
    flash_lock();
    return !memcmp((const void *)SECRET_ADDR, buf, n);
}

static void usart_setup(uint32_t usart) {
    usart_set_baudrate(usart, 115200);
    usart_set_databits(usart, 8);
    usart_set_stopbits(usart, USART_STOPBITS_1);
    usart_set_parity(usart, USART_PARITY_NONE);
    usart_set_flow_control(usart, USART_FLOWCONTROL_NONE);
    usart_set_mode(usart, USART_MODE_TX_RX);
    usart_enable_rx_interrupt(usart);
    usart_enable(usart);
}

static void hw_setup(void) {
#if defined(STM32F4)
    rcc_clock_setup_pll(&rcc_hse_25mhz_3v3[RCC_CLOCK_3V3_84MHZ]); /* Black Pill: кварц 25 МГц */
#else
    rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]); /* Blue Pill: кварц 8 МГц */
#endif
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOC);
    rcc_periph_clock_enable(RCC_USART1);
    rcc_periph_clock_enable(RCC_USART2);

#if defined(STM32F4)
    gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO2 | GPIO9);
    gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO3 | GPIO10);
    gpio_set_af(GPIOA, GPIO_AF7, GPIO2 | GPIO3 | GPIO9 | GPIO10);
    gpio_mode_setup(GPIOC, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO13);
#else
    rcc_periph_clock_enable(RCC_AFIO);
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX | GPIO_USART2_TX);
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_PULL_UPDOWN, GPIO_USART1_RX | GPIO_USART2_RX);
    gpio_set(GPIOA, GPIO_USART1_RX | GPIO_USART2_RX); /* подтяжка вверх */
    gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO13);
#endif

    usart_setup(USART1);
    usart_setup(USART2);
    nvic_enable_irq(NVIC_USART1_IRQ);
    nvic_enable_irq(NVIC_USART2_IRQ);

    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
    systick_set_reload(rcc_ahb_frequency / 1000 - 1);
    systick_interrupt_enable();
    systick_counter_enable();

    uint32_t uid[3];
    desig_get_unique_id(uid);
    rng_state = uid[0] ^ (uid[1] * 2654435761u) ^ (uid[2] << 7) ^ 0x9E3779B9u;
    if (!rng_state) rng_state = 0xA5A5A5A5u;

    adc_setup();
}

int main(void) {
    hw_setup();

    static const smesh_crypto_platform cp = { entropy, secret_load, secret_store, NULL };
    static const smess_platform plat = { radio_write, console_write, millis, random32, NULL, true, &cp };
    app_init(&plat);

    for (;;) {
        int b;
        while ((b = ring_get(&radio_rx)) >= 0) app_radio_byte((uint8_t)b);
        while ((b = ring_get(&console_rx)) >= 0) {
            smesh_crypto_stir(STK_CVR); /* время нажатий с точностью до такта */
            app_console_char((char)b);
        }
        app_poll();

        /* PC13 включается нулём */
        bool on = app_ready() || (ms / 250) % 2;
        if (on) gpio_clear(GPIOC, GPIO13);
        else gpio_set(GPIOC, GPIO13);

        __asm__ volatile("wfi"); /* до следующего прерывания: байт или тик 1 мс */
    }
}
