/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "pb.h"

#include <string.h>

static void put(pb_w *w, uint8_t b) {
    if (w->len < w->cap)
        w->buf[w->len++] = b;
    else
        w->err = true;
}

void pbw_varint(pb_w *w, uint64_t v) {
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        put(w, b | (v ? 0x80 : 0));
    } while (v);
}

void pbw_tag(pb_w *w, uint32_t field, uint8_t wire_type) {
    pbw_varint(w, ((uint64_t)field << 3) | wire_type);
}

void pbw_uint(pb_w *w, uint32_t field, uint64_t v) {
    if (!v) return;
    pbw_tag(w, field, PB_VARINT);
    pbw_varint(w, v);
}

void pbw_fixed32(pb_w *w, uint32_t field, uint32_t v) {
    if (!v) return;
    pbw_tag(w, field, PB_FIXED32);
    for (int i = 0; i < 4; i++) put(w, (uint8_t)(v >> (8 * i)));
}

void pbw_bytes(pb_w *w, uint32_t field, const void *data, size_t n) {
    pbw_tag(w, field, PB_LEN);
    pbw_varint(w, n);
    if (w->err || n > w->cap - w->len) {
        w->err = true;
        return;
    }
    if (n) memcpy(w->buf + w->len, data, n);
    w->len += n;
}

static bool get(pb_r *r, uint8_t *b) {
    if (r->p >= r->end) {
        r->err = true;
        return false;
    }
    *b = *r->p++;
    return true;
}

uint64_t pbr_varint(pb_r *r) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        uint8_t b;
        if (!get(r, &b)) return 0;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
    }
    r->err = true;
    return 0;
}

uint32_t pbr_fixed32(pb_r *r) {
    if (r->end - r->p < 4) {
        r->err = true;
        r->p = r->end;
        return 0;
    }
    uint32_t v = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 |
                 (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24;
    r->p += 4;
    return v;
}

bool pbr_bytes(pb_r *r, const uint8_t **data, size_t *n) {
    uint64_t len = pbr_varint(r);
    if (r->err || len > (uint64_t)(r->end - r->p)) {
        r->err = true;
        r->p = r->end;
        return false;
    }
    *data = r->p;
    *n = (size_t)len;
    r->p += len;
    return true;
}

bool pbr_next(pb_r *r, uint32_t *field, uint8_t *wire_type) {
    if (r->err || r->p >= r->end) return false;
    uint64_t tag = pbr_varint(r);
    if (r->err) return false;
    *field = (uint32_t)(tag >> 3);
    *wire_type = tag & 7;
    if (*field == 0) r->err = true;
    return !r->err;
}

void pbr_skip(pb_r *r, uint8_t wire_type) {
    const uint8_t *d;
    size_t n;
    switch (wire_type) {
    case PB_VARINT: pbr_varint(r); break;
    case PB_FIXED32: pbr_fixed32(r); break;
    case PB_LEN: pbr_bytes(r, &d, &n); break;
    case PB_FIXED64:
        if (r->end - r->p < 8) {
            r->err = true;
            r->p = r->end;
        } else {
            r->p += 8;
        }
        break;
    default: r->err = true; r->p = r->end;
    }
}
