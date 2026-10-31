/*
 * Конверт STORAGE&MESS поверх Meshtastic (порт PRIVATE_APP = 256).
 *
 * Кадр LoRa — 233 байта полезной нагрузки. Конверт серверного ядра
 * (id 16 + адреса 2×32 + подпись 64) съел бы из них 144, поэтому здесь
 * свой, компактный: 19 байт заголовка, 214 байт текста на кадр.
 *
 *   0      магия 'S' (0x53) — отличает нас от других PRIVATE_APP
 *   1      версия (старшие 4 бита) | тип (младшие 4)
 *   2..5   id сообщения, little-endian
 *   6..11  адрес отправителя (6 байт)
 *   12..17 адрес получателя (6 байт; все 0xFF — всем в канале)
 *   18     номер куска (старшие 4) | кусков всего − 1 (младшие 4)
 *   19..   тело
 *
 * Адрес — непрозрачные 6 байт. Без ключей это номер узла Meshtastic
 * (00 00 + номер BE); с закрытым модулем — отпечаток открытого ключа.
 * Маршрут, повторы в эфире и ключ канала — забота Meshtastic.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SMESS_SMESH_H
#define SMESS_SMESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mt.h"

#define SMESH_MAGIC 0x53
#define SMESH_VERSION 1
#define SMESH_HDR 19
#define SMESH_FRAG_BODY (MT_DATA_PAYLOAD_LEN - SMESH_HDR) /* 214 */

#ifndef SMESH_MAX_FRAGS
#define SMESH_MAX_FRAGS 8   /* до 16; 8 × 214 = 1712 байт на сообщение */
#endif
#ifndef SMESH_SLOTS
#define SMESH_SLOTS 2       /* сколько сообщений собирается одновременно */
#endif
#ifndef SMESH_SEEN
#define SMESH_SEEN 32       /* память на повторы */
#endif
#define SMESH_REASM_TIMEOUT_MS 120000u
#define SMESH_MAX_MSG (SMESH_MAX_FRAGS * SMESH_FRAG_BODY)

typedef struct {
    uint8_t b[6];
} smesh_addr;

enum { SMESH_T_TEXT = 1 };

typedef struct {
    uint8_t type;
    uint32_t msg_id;
    smesh_addr from, to;
    uint8_t idx, total;
    const uint8_t *body;
    size_t body_len;
} smesh_frame;

extern const smesh_addr SMESH_ALL;

smesh_addr smesh_addr_from_node(uint32_t node_num);
bool smesh_addr_eq(const smesh_addr *a, const smesh_addr *b);

/* Сколько кусков нужно на тело такой длины; 0 — не влезает. */
unsigned smesh_frag_count(size_t len);

size_t smesh_encode(uint8_t *out, size_t cap, const smesh_frame *f);
bool smesh_decode(const uint8_t *in, size_t n, smesh_frame *f);

/* Сборка кусков и отсев повторов. */
typedef struct {
    bool used;
    uint32_t msg_id, started_ms;
    smesh_addr from;
    uint8_t type, total;
    uint16_t have;
    uint16_t last_len;
    uint8_t data[SMESH_MAX_MSG];
} smesh_slot;

typedef struct {
    smesh_slot slot[SMESH_SLOTS];
    struct {
        smesh_addr from;
        uint32_t id;
        bool used;
    } seen[SMESH_SEEN];
    uint8_t seen_pos;
} smesh_rx;

void smesh_rx_init(smesh_rx *rx);
/*
 * > 0 — собрано сообщение такой длины (лежит в out);
 *   0 — кусок принят, ждём остальные;
 *  -1 — повтор уже собранного или испорченный кусок.
 */
int smesh_rx_push(smesh_rx *rx, const smesh_frame *f, uint32_t now_ms,
                  uint8_t *out, size_t cap);

#endif
