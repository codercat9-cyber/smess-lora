/*
 * Минимальный protobuf: ровно то, что нужно для Serial API Meshtastic.
 *
 * Готовые генераторы (nanopb) тянут за собой .proto, генерацию и десятки
 * килобайт кода. Нам нужно пять сообщений и четыре типа полей, поэтому
 * кодек написан руками: без кучи, без зависимостей, одинаково на ПК и MCU.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SMESS_PB_H
#define SMESS_PB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PB_VARINT = 0, PB_FIXED64 = 1, PB_LEN = 2, PB_FIXED32 = 5 };

/* Писатель. При нехватке места выставляет err и дальше молчит. */
typedef struct {
    uint8_t *buf;
    size_t cap, len;
    bool err;
} pb_w;

void pbw_varint(pb_w *w, uint64_t v);
void pbw_tag(pb_w *w, uint32_t field, uint8_t wire_type);
/* Нулевые значения не пишутся — как в proto3. */
void pbw_uint(pb_w *w, uint32_t field, uint64_t v);
void pbw_fixed32(pb_w *w, uint32_t field, uint32_t v);
/* Пишется всегда, даже пустое: пустое вложенное сообщение — тоже значение. */
void pbw_bytes(pb_w *w, uint32_t field, const void *data, size_t n);

/* Читатель. Любая порча входа — err, без выхода за границы буфера. */
typedef struct {
    const uint8_t *p, *end;
    bool err;
} pb_r;

static inline pb_r pbr_init(const uint8_t *d, size_t n) {
    pb_r r = { d, d + n, false };
    return r;
}
bool pbr_next(pb_r *r, uint32_t *field, uint8_t *wire_type);
uint64_t pbr_varint(pb_r *r);
uint32_t pbr_fixed32(pb_r *r);
bool pbr_bytes(pb_r *r, const uint8_t **data, size_t *n);
void pbr_skip(pb_r *r, uint8_t wire_type);

#endif
