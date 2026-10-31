/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "smesh.h"

#include <string.h>

const smesh_addr SMESH_ALL = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };

smesh_addr smesh_addr_from_node(uint32_t n) {
    smesh_addr a = { { 0, 0, (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n } };
    return a;
}

bool smesh_addr_eq(const smesh_addr *a, const smesh_addr *b) {
    return memcmp(a->b, b->b, sizeof a->b) == 0;
}

unsigned smesh_frag_count(size_t len) {
    unsigned n = len ? (unsigned)((len + SMESH_FRAG_BODY - 1) / SMESH_FRAG_BODY) : 1;
    return n <= SMESH_MAX_FRAGS ? n : 0;
}

size_t smesh_encode(uint8_t *out, size_t cap, const smesh_frame *f) {
    if (f->total == 0 || f->total > 16 || f->idx >= f->total) return 0;
    if (f->body_len > SMESH_FRAG_BODY || SMESH_HDR + f->body_len > cap) return 0;
    out[0] = SMESH_MAGIC;
    out[1] = (uint8_t)(SMESH_VERSION << 4 | (f->type & 0x0F));
    for (int i = 0; i < 4; i++) out[2 + i] = (uint8_t)(f->msg_id >> (8 * i));
    memcpy(out + 6, f->from.b, 6);
    memcpy(out + 12, f->to.b, 6);
    out[18] = (uint8_t)(f->idx << 4 | (f->total - 1));
    if (f->body_len) memcpy(out + SMESH_HDR, f->body, f->body_len);
    return SMESH_HDR + f->body_len;
}

bool smesh_decode(const uint8_t *in, size_t n, smesh_frame *f) {
    if (n < SMESH_HDR || in[0] != SMESH_MAGIC || (in[1] >> 4) != SMESH_VERSION) return false;
    memset(f, 0, sizeof *f);
    f->type = in[1] & 0x0F;
    f->msg_id = (uint32_t)in[2] | (uint32_t)in[3] << 8 | (uint32_t)in[4] << 16 | (uint32_t)in[5] << 24;
    memcpy(f->from.b, in + 6, 6);
    memcpy(f->to.b, in + 12, 6);
    f->idx = in[18] >> 4;
    f->total = (uint8_t)((in[18] & 0x0F) + 1);
    f->body = in + SMESH_HDR;
    f->body_len = n - SMESH_HDR;
    return f->idx < f->total && f->body_len <= SMESH_FRAG_BODY;
}

void smesh_rx_init(smesh_rx *rx) {
    memset(rx, 0, sizeof *rx);
}

static bool seen(const smesh_rx *rx, const smesh_addr *from, uint32_t id) {
    for (int i = 0; i < SMESH_SEEN; i++)
        if (rx->seen[i].used && rx->seen[i].id == id && smesh_addr_eq(&rx->seen[i].from, from)) return true;
    return false;
}

static void remember(smesh_rx *rx, const smesh_addr *from, uint32_t id) {
    rx->seen[rx->seen_pos].from = *from;
    rx->seen[rx->seen_pos].id = id;
    rx->seen[rx->seen_pos].used = true;
    rx->seen_pos = (uint8_t)((rx->seen_pos + 1) % SMESH_SEEN);
}

int smesh_rx_push(smesh_rx *rx, const smesh_frame *f, uint32_t now, uint8_t *out, size_t cap) {
    if (f->total > SMESH_MAX_FRAGS || f->idx >= f->total) return -1;
    if (seen(rx, &f->from, f->msg_id)) return -1;

    /* Все куски, кроме последнего, полные — иначе смещения не сойдутся */
    bool last = f->idx == f->total - 1;
    if (!last && f->body_len != SMESH_FRAG_BODY) return -1;

    if (f->total == 1) {
        if (f->body_len > cap) return -1;
        memcpy(out, f->body, f->body_len);
        remember(rx, &f->from, f->msg_id);
        return (int)f->body_len;
    }

    smesh_slot *s = NULL, *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < SMESH_SLOTS; i++) {
        smesh_slot *c = &rx->slot[i];
        if (c->used && now - c->started_ms > SMESH_REASM_TIMEOUT_MS) c->used = false;
        if (c->used && c->msg_id == f->msg_id && smesh_addr_eq(&c->from, &f->from)) s = c;
        if (!c->used && !free_slot) free_slot = c;
        if (c->used && (!oldest || now - c->started_ms > now - oldest->started_ms)) oldest = c;
    }
    if (!s) {
        /* Мест нет — жертвуем самой старой недособранной */
        s = free_slot ? free_slot : oldest;
        memset(s, 0, offsetof(smesh_slot, data));
        s->used = true;
        s->msg_id = f->msg_id;
        s->from = f->from;
        s->type = f->type;
        s->total = f->total;
        s->started_ms = now;
    }
    if (s->total != f->total) return -1;

    memcpy(s->data + (size_t)f->idx * SMESH_FRAG_BODY, f->body, f->body_len);
    if (last) s->last_len = (uint16_t)f->body_len;
    s->have |= (uint16_t)(1u << f->idx);
    if (s->have != (uint16_t)((1u << s->total) - 1)) return 0;

    size_t len = (size_t)(s->total - 1) * SMESH_FRAG_BODY + s->last_len;
    s->used = false;
    if (len > cap) return -1;
    memcpy(out, s->data, len);
    remember(rx, &f->from, f->msg_id);
    return (int)len;
}
