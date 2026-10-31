/*
 * Клиент STORAGE&MESS для LoRa: логика без привязки к железу.
 *
 * Платформа (Linux, STM32) даёт четыре вещи: записать в радио, записать в
 * консоль, время в мс и случайное число. Всё остальное — здесь: подключение
 * к узлу Meshtastic, разбор кадров, команды, сборка и отправка сообщений.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SMESS_APP_H
#define SMESS_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void (*radio_write)(const uint8_t *data, size_t len);
    void (*console_write)(const char *text);
    uint32_t (*millis)(void);
    uint32_t (*random32)(void);
    void (*radio_log)(uint8_t b); /* текстовый журнал узла, может быть NULL */
    bool echo;                    /* эхо ввода: нужно на UART-терминале, не нужно в Linux */
} smess_platform;

void app_init(const smess_platform *p);
void app_radio_byte(uint8_t b);
void app_console_char(char c);
void app_poll(void);
bool app_ready(void);

#endif
