/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "mt.h"

#include <string.h>

#include "pb.h"

/* Номера полей — mesh.proto */
enum { TR_PACKET = 1, TR_WANT_CONFIG = 3, TR_HEARTBEAT = 7 };
enum {
    FR_ID = 1, FR_PACKET = 2, FR_MY_INFO = 3, FR_NODE_INFO = 4,
    FR_CONFIG_COMPLETE = 7, FR_REBOOTED = 8, FR_QUEUE_STATUS = 11
};
enum {
    MP_FROM = 1, MP_TO = 2, MP_CHANNEL = 3, MP_DECODED = 4, MP_ENCRYPTED = 5,
    MP_ID = 6, MP_RX_TIME = 7, MP_RX_SNR = 8, MP_HOP_LIMIT = 9, MP_WANT_ACK = 10,
    MP_RX_RSSI = 12, MP_HOP_START = 15
};
enum { DA_PORTNUM = 1, DA_PAYLOAD = 2, DA_REQUEST_ID = 6 };

static float as_float(uint32_t v) {
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Поле с неожиданным типом провода пропускаем, а не читаем как попало. */
#define WANT(t) if (wt != (t)) { pbr_skip(&r, wt); break; }

static bool dec_data(const uint8_t *d, size_t n, mt_packet *p) {
    pb_r r = pbr_init(d, n);
    uint32_t f;
    uint8_t wt;
    while (pbr_next(&r, &f, &wt)) {
        switch (f) {
        case DA_PORTNUM: WANT(PB_VARINT) p->portnum = (uint32_t)pbr_varint(&r); break;
        case DA_PAYLOAD: WANT(PB_LEN) pbr_bytes(&r, &p->payload, &p->payload_len); break;
        case DA_REQUEST_ID: WANT(PB_FIXED32) p->request_id = pbr_fixed32(&r); break;
        default: pbr_skip(&r, wt);
        }
    }
    return !r.err;
}

static bool dec_packet(const uint8_t *d, size_t n, mt_packet *p) {
    pb_r r = pbr_init(d, n);
    uint32_t f;
    uint8_t wt;
    const uint8_t *sub;
    size_t sn;
    memset(p, 0, sizeof *p);
    while (pbr_next(&r, &f, &wt)) {
        switch (f) {
        case MP_FROM: WANT(PB_FIXED32) p->from = pbr_fixed32(&r); break;
        case MP_TO: WANT(PB_FIXED32) p->to = pbr_fixed32(&r); break;
        case MP_CHANNEL: WANT(PB_VARINT) p->channel = (uint32_t)pbr_varint(&r); break;
        case MP_DECODED:
            WANT(PB_LEN)
            if (pbr_bytes(&r, &sub, &sn)) p->has_decoded = dec_data(sub, sn, p);
            break;
        case MP_ENCRYPTED: WANT(PB_LEN) p->encrypted = true; pbr_skip(&r, wt); break;
        case MP_ID: WANT(PB_FIXED32) p->id = pbr_fixed32(&r); break;
        case MP_RX_TIME: WANT(PB_FIXED32) p->rx_time = pbr_fixed32(&r); break;
        case MP_RX_SNR: WANT(PB_FIXED32) p->rx_snr = as_float(pbr_fixed32(&r)); break;
        case MP_HOP_LIMIT: WANT(PB_VARINT) p->hop_limit = (uint32_t)pbr_varint(&r); break;
        case MP_WANT_ACK: WANT(PB_VARINT) p->want_ack = pbr_varint(&r) != 0; break;
        case MP_RX_RSSI: WANT(PB_VARINT) p->rx_rssi = (int32_t)(uint32_t)pbr_varint(&r); break;
        case MP_HOP_START: WANT(PB_VARINT) p->hop_start = (uint32_t)pbr_varint(&r); break;
        default: pbr_skip(&r, wt);
        }
    }
    return !r.err;
}

static bool dec_user(const uint8_t *d, size_t n, mt_node *nd) {
    pb_r r = pbr_init(d, n);
    uint32_t f;
    uint8_t wt;
    const uint8_t *s;
    size_t sn;
    while (pbr_next(&r, &f, &wt)) {
        if (wt == PB_LEN && f >= 1 && f <= 3) {
            if (!pbr_bytes(&r, &s, &sn)) break;
            if (f == 1) copy_str(nd->id, sizeof nd->id, s, sn);
            if (f == 2) copy_str(nd->long_name, sizeof nd->long_name, s, sn);
            if (f == 3) copy_str(nd->short_name, sizeof nd->short_name, s, sn);
        } else {
            pbr_skip(&r, wt);
        }
    }
    return !r.err;
}

static bool dec_node(const uint8_t *d, size_t n, mt_node *nd) {
    pb_r r = pbr_init(d, n);
    uint32_t f;
    uint8_t wt;
    const uint8_t *sub;
    size_t sn;
    while (pbr_next(&r, &f, &wt)) {
        switch (f) {
        case 1: WANT(PB_VARINT) nd->num = (uint32_t)pbr_varint(&r); break;
        case 2: WANT(PB_LEN) if (pbr_bytes(&r, &sub, &sn)) dec_user(sub, sn, nd); break;
        case 4: WANT(PB_FIXED32) nd->snr = as_float(pbr_fixed32(&r)); break;
        case 5: WANT(PB_FIXED32) nd->last_heard = pbr_fixed32(&r); break;
        case 9: WANT(PB_VARINT) nd->hops_away = (uint32_t)pbr_varint(&r); break;
        default: pbr_skip(&r, wt);
        }
    }
    return !r.err;
}

bool mt_decode_fromradio(const uint8_t *buf, size_t len, mt_fromradio *o) {
    pb_r r = pbr_init(buf, len);
    uint32_t f;
    uint8_t wt;
    const uint8_t *sub;
    size_t sn;
    memset(o, 0, sizeof *o);
    while (pbr_next(&r, &f, &wt)) {
        switch (f) {
        case FR_ID: WANT(PB_VARINT) o->id = (uint32_t)pbr_varint(&r); break;
        case FR_PACKET:
            WANT(PB_LEN)
            if (pbr_bytes(&r, &sub, &sn) && dec_packet(sub, sn, &o->packet)) o->kind = MT_FR_PACKET;
            break;
        case FR_MY_INFO: {
            WANT(PB_LEN)
            if (!pbr_bytes(&r, &sub, &sn)) break;
            pb_r m = pbr_init(sub, sn);
            uint32_t mf;
            uint8_t mwt;
            while (pbr_next(&m, &mf, &mwt)) {
                if (mf == 1 && mwt == PB_VARINT) o->my_node_num = (uint32_t)pbr_varint(&m);
                else pbr_skip(&m, mwt);
            }
            if (!m.err) o->kind = MT_FR_MY_INFO;
            break;
        }
        case FR_NODE_INFO:
            WANT(PB_LEN)
            if (pbr_bytes(&r, &sub, &sn) && dec_node(sub, sn, &o->node)) o->kind = MT_FR_NODE_INFO;
            break;
        case FR_CONFIG_COMPLETE:
            WANT(PB_VARINT)
            o->config_complete_id = (uint32_t)pbr_varint(&r);
            o->kind = MT_FR_CONFIG_COMPLETE;
            break;
        case FR_REBOOTED: WANT(PB_VARINT) pbr_varint(&r); o->kind = MT_FR_REBOOTED; break;
        case FR_QUEUE_STATUS: {
            WANT(PB_LEN)
            if (!pbr_bytes(&r, &sub, &sn)) break;
            pb_r q = pbr_init(sub, sn);
            uint32_t qf;
            uint8_t qwt;
            while (pbr_next(&q, &qf, &qwt)) {
                if (qwt != PB_VARINT) { pbr_skip(&q, qwt); continue; }
                uint64_t v = pbr_varint(&q);
                if (qf == 1) o->queue.res = (int32_t)(uint32_t)v;
                else if (qf == 2) o->queue.free = (uint32_t)v;
                else if (qf == 3) o->queue.maxlen = (uint32_t)v;
                else if (qf == 4) o->queue.mesh_packet_id = (uint32_t)v;
            }
            if (!q.err) o->kind = MT_FR_QUEUE_STATUS;
            break;
        }
        default: pbr_skip(&r, wt);
        }
    }
    return !r.err;
}

static size_t finish(uint8_t *out, const pb_w *w) {
    if (w->err || w->len > MT_MAX_PAYLOAD) return 0;
    out[0] = MT_START1;
    out[1] = MT_START2;
    out[2] = (uint8_t)(w->len >> 8);
    out[3] = (uint8_t)(w->len & 0xFF);
    return w->len + 4;
}

static pb_w body_writer(uint8_t *out, size_t cap) {
    size_t room = cap > 4 ? cap - 4 : 0;
    pb_w w = { out + 4, room > MT_MAX_PAYLOAD ? MT_MAX_PAYLOAD : room, 0, cap < 4 };
    return w;
}

size_t mt_frame_want_config(uint8_t *out, size_t cap, uint32_t config_id) {
    pb_w w = body_writer(out, cap);
    if (w.err || !config_id) return 0;
    pbw_uint(&w, TR_WANT_CONFIG, config_id);
    return finish(out, &w);
}

size_t mt_frame_heartbeat(uint8_t *out, size_t cap, uint32_t nonce) {
    uint8_t hb[8];
    pb_w h = { hb, sizeof hb, 0, false };
    pbw_uint(&h, 1, nonce);
    pb_w w = body_writer(out, cap);
    if (w.err) return 0;
    pbw_bytes(&w, TR_HEARTBEAT, hb, h.len);
    return finish(out, &w);
}

size_t mt_frame_send(uint8_t *out, size_t cap, const mt_send *s) {
    if (s->len > MT_DATA_PAYLOAD_LEN || !s->id) return 0;

    uint8_t data[MT_DATA_PAYLOAD_LEN + 16];
    pb_w dw = { data, sizeof data, 0, false };
    pbw_uint(&dw, DA_PORTNUM, s->portnum);
    if (s->len) pbw_bytes(&dw, DA_PAYLOAD, s->payload, s->len);

    uint8_t pkt[sizeof data + 32];
    pb_w pw = { pkt, sizeof pkt, 0, false };
    pbw_fixed32(&pw, MP_TO, s->to);
    pbw_uint(&pw, MP_CHANNEL, s->channel);
    pbw_bytes(&pw, MP_DECODED, data, dw.len);
    pbw_fixed32(&pw, MP_ID, s->id);
    pbw_uint(&pw, MP_HOP_LIMIT, s->hop_limit);
    pbw_uint(&pw, MP_WANT_ACK, s->want_ack);
    if (dw.err || pw.err) return 0;

    pb_w w = body_writer(out, cap);
    if (w.err) return 0;
    pbw_bytes(&w, TR_PACKET, pkt, pw.len);
    return finish(out, &w);
}

void mt_wake_bytes(uint8_t out[MT_WAKE_LEN]) {
    memset(out, MT_START2, MT_WAKE_LEN);
}

void mt_parser_init(mt_parser *p, void (*on_log)(uint8_t, void *), void *ctx) {
    memset(p, 0, sizeof *p);
    p->on_log = on_log;
    p->ctx = ctx;
}

static void log_byte(mt_parser *p, uint8_t b) {
    if (p->on_log) p->on_log(b, p->ctx);
}

int mt_parser_feed(mt_parser *p, uint8_t b) {
    switch (p->state) {
    case 0:
        if (b == MT_START1) p->state = 1;
        else log_byte(p, b);
        return 0;
    case 1:
        if (b == MT_START2) {
            p->state = 2;
            return 0;
        }
        /* 0x94 оказался частью текста — отдаём его и разбираем байт заново */
        p->state = 0;
        log_byte(p, MT_START1);
        return mt_parser_feed(p, b);
    case 2:
        p->need = (uint16_t)(b << 8);
        p->state = 3;
        return 0;
    case 3:
        p->need |= b;
        p->got = 0;
        /* Длина больше допустимой — это не кадр, а мусор: ищем начало снова */
        p->state = (p->need == 0 || p->need > MT_MAX_PAYLOAD) ? 0 : 4;
        return 0;
    default:
        p->buf[p->got++] = b;
        if (p->got < p->need) return 0;
        p->state = 0;
        return p->need;
    }
}
