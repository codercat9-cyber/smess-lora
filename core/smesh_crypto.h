/*
 * Шифрование содержимого сообщений — точка подключения.
 *
 * Сами механизмы (ключи, обмен, шифр) — закрытая часть STORAGE&MESS и в
 * открытый код не входят. Здесь только интерфейс; в открытой сборке стоит
 * crypto_null.c: тело идёт как есть, а в эфире его закрывает лишь ключ
 * канала Meshtastic. Закрытая реализация собирается так: make CRYPTO=private
 * (берётся private/crypto.c, в репозиторий он не попадает).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SMESS_SMESH_CRYPTO_H
#define SMESS_SMESH_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "smesh.h"

/* Вызывается один раз при старте; node_num — номер своего узла. */
void smesh_crypto_init(uint32_t node_num);

/* Свой адрес: без ключей — производный от номера узла. */
smesh_addr smesh_crypto_self(void);

/* Адрес собеседника по номеру его узла (с ключами — из своей адресной книги). */
smesh_addr smesh_crypto_peer(uint32_t node_num);

/* Сколько байт добавляет запечатывание (заголовок, тег). */
size_t smesh_crypto_overhead(void);

/* Запечатать тело для адресата. Возвращает длину или 0 при ошибке. */
size_t smesh_seal(const smesh_addr *to, const uint8_t *in, size_t n, uint8_t *out, size_t cap);

/* Открыть тело от отправителя. -1 — не открывается (чужое, испорчено). */
long smesh_open(const smesh_addr *from, const smesh_addr *to, const uint8_t *in, size_t n,
                uint8_t *out, size_t cap);

/* Короткое имя механизма для /info: "нет" в открытой сборке. */
const char *smesh_crypto_name(void);

#endif
