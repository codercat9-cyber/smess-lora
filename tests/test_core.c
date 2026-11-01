/*
 * Проверки ядра на ПК: protobuf, разбор потока, конверт, сборка кусков,
 * и сквозной прогон клиента через поддельный узел Meshtastic.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "mt.h"
#include "pb.h"
#include "smesh.h"
#include "smesh_crypto.h"

static int fails, checks;
#define CHECK(c)                                                              \
    do {                                                                      \
        checks++;                                                             \
        if (!(c)) {                                                           \
            fails++;                                                          \
            printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
        }                                                                     \
    } while (0)

/* --- protobuf --- */
static void test_pb(void) {
    uint8_t b[64];
    pb_w w = { b, sizeof b, 0, false };
    pbw_uint(&w, 1, 300);
    pbw_fixed32(&w, 2, 0xDEADBEEF);
    pbw_bytes(&w, 3, "abc", 3);
    pbw_uint(&w, 4, 0); /* ноль не пишется */
    CHECK(!w.err);
    pb_r r = pbr_init(b, w.len);
    uint32_t f;
    uint8_t wt;
    CHECK(pbr_next(&r, &f, &wt) && f == 1 && wt == PB_VARINT && pbr_varint(&r) == 300);
    CHECK(pbr_next(&r, &f, &wt) && f == 2 && pbr_fixed32(&r) == 0xDEADBEEF);
    const uint8_t *d;
    size_t n;
    CHECK(pbr_next(&r, &f, &wt) && f == 3 && pbr_bytes(&r, &d, &n) && n == 3 && !memcmp(d, "abc", 3));
    CHECK(!pbr_next(&r, &f, &wt) && !r.err);

    /* Обрезанный вход не читается за границу */
    pb_r bad = pbr_init(b, 5);
    while (pbr_next(&bad, &f, &wt)) pbr_skip(&bad, wt);
    CHECK(bad.err);

    /* Переполнение писателя */
    uint8_t tiny[3];
    pb_w t = { tiny, sizeof tiny, 0, false };
    pbw_bytes(&t, 1, "hello", 5);
    CHECK(t.err);
}

/* --- поток кадров --- */
static int logged;
static void count_log(uint8_t b, void *ctx) {
    (void)b;
    (void)ctx;
    logged++;
}

static void test_parser(void) {
    mt_parser p;
    mt_parser_init(&p, count_log, NULL);
    uint8_t frame[64];
    size_t n = mt_frame_want_config(frame, sizeof frame, 42);
    CHECK(n == 6 && frame[0] == 0x94 && frame[1] == 0xC3 && frame[3] == 2);

    /* Журнал узла, ложное начало 0x94, мусорная длина — и потом настоящий кадр */
    const uint8_t junk[] = { 'I', 'N', 'F', 'O', 0x94, 'x', 0x94, 0xC3, 0x7F, 0xFF, '\n' };
    int got = 0;
    for (size_t i = 0; i < sizeof junk; i++) got |= mt_parser_feed(&p, junk[i]);
    CHECK(got == 0);
    for (size_t i = 0; i < n; i++) got = mt_parser_feed(&p, frame[i]);
    CHECK(got == 2 && p.buf[0] == 0x18 && p.buf[1] == 42);
    CHECK(logged >= 6);
}

/* --- конверт и сборка --- */
static void test_smesh(void) {
    uint8_t buf[MT_DATA_PAYLOAD_LEN];
    smesh_frame f = { SMESH_T_TEXT, 0x01020304, smesh_addr_from_node(0xA1B2C3D4), SMESH_ALL, 2, 3,
                      (const uint8_t *)"hi", 2 };
    size_t n = smesh_encode(buf, sizeof buf, &f);
    CHECK(n == SMESH_HDR + 2);
    smesh_frame g;
    CHECK(smesh_decode(buf, n, &g));
    CHECK(g.msg_id == 0x01020304 && g.idx == 2 && g.total == 3 && g.body_len == 2);
    CHECK(smesh_addr_eq(&g.from, &f.from) && smesh_addr_eq(&g.to, &SMESH_ALL));
    buf[0] = 'X';
    CHECK(!smesh_decode(buf, n, &g));

    CHECK(smesh_frag_count(0) == 1 && smesh_frag_count(SMESH_FRAG_BODY) == 1);
    CHECK(smesh_frag_count(SMESH_FRAG_BODY + 1) == 2 && smesh_frag_count(SMESH_MAX_MSG + 1) == 0);

    /* Три куска вразнобой + повтор куска + повтор целого сообщения */
    static uint8_t msg[3 * SMESH_FRAG_BODY - 10], outb[SMESH_MAX_MSG];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i * 7);
    smesh_rx rx;
    smesh_rx_init(&rx);
    smesh_addr from = smesh_addr_from_node(7);
    int order[] = { 2, 0, 0, 1 };
    int res = 0;
    for (int k = 0; k < 4; k++) {
        int i = order[k];
        size_t off = (size_t)i * SMESH_FRAG_BODY;
        size_t bl = i == 2 ? sizeof msg - off : SMESH_FRAG_BODY;
        smesh_frame part = { SMESH_T_TEXT, 99, from, SMESH_ALL, (uint8_t)i, 3, msg + off, bl };
        res = smesh_rx_push(&rx, &part, 1000, outb, sizeof outb);
        if (k < 3) CHECK(res == 0);
    }
    CHECK(res == (int)sizeof msg && !memcmp(outb, msg, sizeof msg));
    smesh_frame again = { SMESH_T_TEXT, 99, from, SMESH_ALL, 0, 3, msg, SMESH_FRAG_BODY };
    CHECK(smesh_rx_push(&rx, &again, 2000, outb, sizeof outb) == -1);

    /* Недособранное устаревает */
    smesh_frame lone = { SMESH_T_TEXT, 5, from, SMESH_ALL, 0, 2, msg, SMESH_FRAG_BODY };
    CHECK(smesh_rx_push(&rx, &lone, 0, outb, sizeof outb) == 0);
    smesh_frame tail = { SMESH_T_TEXT, 5, from, SMESH_ALL, 1, 2, msg, 4 };
    CHECK(smesh_rx_push(&rx, &tail, SMESH_REASM_TIMEOUT_MS + 10, outb, sizeof outb) == 0);

    /* Неполный не последний кусок — порча */
    smesh_frame shortp = { SMESH_T_TEXT, 6, from, SMESH_ALL, 0, 2, msg, 10 };
    CHECK(smesh_rx_push(&rx, &shortp, 0, outb, sizeof outb) == -1);
}

/* --- сквозной прогон клиента через поддельный узел --- */
static uint32_t now_ms;
static char console[16384];
static size_t console_len;
static uint8_t radio[8192];
static size_t radio_len;
static uint32_t rnd = 12345;

static void fake_radio_write(const uint8_t *d, size_t n) {
    if (radio_len + n <= sizeof radio) memcpy(radio + radio_len, d, n), radio_len += n;
}
static void fake_console(const char *s) {
    size_t n = strlen(s);
    if (console_len + n < sizeof console) memcpy(console + console_len, s, n + 1), console_len += n;
}
static uint32_t fake_millis(void) { return now_ms; }
static uint32_t fake_rand(void) { return rnd = rnd * 1103515245u + 12345u; }

/* Достаёт из того, что клиент отправил узлу, кадры ToRadio */
typedef struct {
    uint32_t want_config, to, port, id;
    bool want_ack, heartbeat;
    uint8_t payload[MT_DATA_PAYLOAD_LEN];
    size_t len;
} toradio;

static int pull_toradio(toradio *outv, int max) {
    mt_parser p;
    mt_parser_init(&p, NULL, NULL);
    int k = 0;
    for (size_t i = 0; i < radio_len && k < max; i++) {
        int n = mt_parser_feed(&p, radio[i]);
        if (n <= 0) continue;
        toradio *t = &outv[k++];
        memset(t, 0, sizeof *t);
        pb_r r = pbr_init(p.buf, (size_t)n);
        uint32_t f;
        uint8_t wt;
        const uint8_t *sub, *sub2;
        size_t sn, sn2;
        while (pbr_next(&r, &f, &wt)) {
            if (f == 3) t->want_config = (uint32_t)pbr_varint(&r);
            else if (f == 7) t->heartbeat = true, pbr_skip(&r, wt);
            else if (f == 1 && pbr_bytes(&r, &sub, &sn)) {
                pb_r m = pbr_init(sub, sn);
                while (pbr_next(&m, &f, &wt)) {
                    if (f == 2) t->to = pbr_fixed32(&m);
                    else if (f == 6) t->id = pbr_fixed32(&m);
                    else if (f == 10) t->want_ack = pbr_varint(&m);
                    else if (f == 4 && pbr_bytes(&m, &sub2, &sn2)) {
                        pb_r d = pbr_init(sub2, sn2);
                        while (pbr_next(&d, &f, &wt)) {
                            if (f == 1) t->port = (uint32_t)pbr_varint(&d);
                            else if (f == 2 && pbr_bytes(&d, &sub, &sn)) memcpy(t->payload, sub, sn), t->len = sn;
                            else pbr_skip(&d, wt);
                        }
                    } else pbr_skip(&m, wt);
                }
            } else pbr_skip(&r, wt);
        }
    }
    radio_len = 0;
    return k;
}

/* Поддельный узел: шлёт клиенту FromRadio */
static void feed_fromradio(const uint8_t *body, size_t n) {
    uint8_t hdr[4] = { 0x94, 0xC3, (uint8_t)(n >> 8), (uint8_t)n };
    for (int i = 0; i < 4; i++) app_radio_byte(hdr[i]);
    for (size_t i = 0; i < n; i++) app_radio_byte(body[i]);
}

static void node_says(uint32_t field, const uint8_t *msg, size_t n) {
    uint8_t b[400];
    pb_w w = { b, sizeof b, 0, false };
    pbw_uint(&w, 1, 77);
    pbw_bytes(&w, field, msg, n);
    feed_fromradio(b, w.len);
}

static void node_packet(uint32_t from, uint32_t to, uint32_t port, const uint8_t *pl, size_t n) {
    uint8_t data[300], pkt[340];
    pb_w d = { data, sizeof data, 0, false };
    pbw_uint(&d, 1, port);
    pbw_bytes(&d, 2, pl, n);
    pb_w p = { pkt, sizeof pkt, 0, false };
    pbw_fixed32(&p, 1, from);
    pbw_fixed32(&p, 2, to);
    pbw_bytes(&p, 4, data, d.len);
    pbw_fixed32(&p, 6, 0x5555);
    node_says(2, pkt, p.len);
}

static void type_line(const char *s) {
    while (*s) app_console_char(*s++);
    app_console_char('\n');
}

static void test_app(void) {
    static const smess_platform plat = { fake_radio_write, fake_console, fake_millis, fake_rand, NULL, false, NULL };
    const uint32_t ME = 0xA1B2C3D4, PEER = 0x0BADF00D;
    toradio tr[16];

    app_init(&plat);
    CHECK(!app_ready());
    CHECK(pull_toradio(tr, 16) == 0 && 1); /* сначала только байты пробуждения, кадров нет */
    now_ms += 200;
    app_poll();
    CHECK(pull_toradio(tr, 16) == 1 && tr[0].want_config != 0);
    uint32_t cfg = tr[0].want_config;

    /* Узел рассказывает о себе и о соседе, затем «конфиг весь» */
    uint8_t mi[8];
    pb_w w = { mi, sizeof mi, 0, false };
    pbw_uint(&w, 1, ME);
    node_says(3, mi, w.len);
    uint8_t user[64], ni[96];
    pb_w u = { user, sizeof user, 0, false };
    pbw_bytes(&u, 2, "Peer Node", 9);
    pbw_bytes(&u, 3, "peer", 4);
    pb_w n = { ni, sizeof ni, 0, false };
    pbw_uint(&n, 1, PEER);
    pbw_fixed32(&n, 5, 1761999999u); /* слышали только что */
    pbw_bytes(&n, 2, user, u.len);
    node_says(4, ni, n.len);
    /* Свой узел и толпа соседей сверх таблицы — как на живой сети в 200
     * узлов. Свой не должен вытесниться (у него last_heard = 0). */
    uint8_t self_user[64], self_ni[96];
    pb_w su = { self_user, sizeof self_user, 0, false };
    pbw_bytes(&su, 2, "Bimzh", 5);
    pbw_bytes(&su, 3, "bmzh", 4);
    pb_w sn = { self_ni, sizeof self_ni, 0, false };
    pbw_uint(&sn, 1, ME);
    pbw_bytes(&sn, 2, self_user, su.len);
    node_says(4, self_ni, sn.len);
    for (uint32_t k = 0; k < 60; k++) {
        uint8_t crowd[32];
        pb_w cw = { crowd, sizeof crowd, 0, false };
        pbw_uint(&cw, 1, 0x100 + k);
        pbw_fixed32(&cw, 5, 1761900000u + k);
        node_says(4, crowd, cw.len);
    }
    node_says(4, ni, n.len); /* сосед «peer» — самый свежий */

    uint8_t cc[8];
    pb_w c = { cc, sizeof cc, 0, false };
    pbw_uint(&c, 7, cfg);
    feed_fromradio(cc, c.len);
    CHECK(app_ready());
    CHECK(strstr(console, "Готов") != NULL);
    CHECK(strstr(console, "(bmzh)") != NULL); /* своё имя пережило вытеснение */

    /* Короткое сообщение лично соседу — один кадр PRIVATE_APP с want_ack */
    type_line("/to peer");
    type_line("привет, это STM32");
    app_poll();
    int k = pull_toradio(tr, 16);
    CHECK(k == 1 && tr[0].to == PEER && tr[0].port == MT_PORT_PRIVATE && tr[0].want_ack);
    smesh_frame f;
    CHECK(smesh_decode(tr[0].payload, tr[0].len, &f) && f.total == 1);
    CHECK(f.body_len == strlen("привет, это STM32") && !memcmp(f.body, "привет, это STM32", f.body_len));
    smesh_addr me_addr = smesh_addr_from_node(ME), peer_addr = smesh_addr_from_node(PEER);
    CHECK(smesh_addr_eq(&f.from, &me_addr) && smesh_addr_eq(&f.to, &peer_addr));

    /* Длинное всем — несколько кадров с паузой между ними, по порядку */
    static char longmsg[600];
    for (int i = 0; i < 599; i++) longmsg[i] = (char)('a' + i % 26);
    type_line("/to all");
    type_line(longmsg);
    int frames = 0;
    for (int step = 0; step < 10; step++) {
        app_poll();
        frames += pull_toradio(tr + frames, 16 - frames);
        now_ms += 1600;
    }
    CHECK(frames == 3);
    for (int i = 0; i < frames; i++) {
        CHECK(tr[i].to == MT_BROADCAST && !tr[i].want_ack);
        CHECK(smesh_decode(tr[i].payload, tr[i].len, &f) && f.idx == i && f.total == 3);
    }

    /* Ответ соседа приходит обратно — и печатается */
    console_len = 0;
    console[0] = 0;
    uint8_t fr[MT_DATA_PAYLOAD_LEN];
    smesh_frame reply = { SMESH_T_TEXT, 4242, peer_addr, me_addr, 0, 1, (const uint8_t *)"ответ", strlen("ответ") };
    size_t fl = smesh_encode(fr, sizeof fr, &reply);
    node_packet(PEER, ME, MT_PORT_PRIVATE, fr, fl);
    CHECK(strstr(console, "<- peer: ответ") != NULL);
    /* Повтор того же — не печатается второй раз */
    console_len = 0;
    console[0] = 0;
    node_packet(PEER, ME, MT_PORT_PRIVATE, fr, fl);
    CHECK(console_len == 0);
    /* Чужое (другому адресату) — молча мимо */
    smesh_frame other = { SMESH_T_TEXT, 4243, peer_addr, smesh_addr_from_node(1), 0, 1, (const uint8_t *)"x", 1 };
    fl = smesh_encode(fr, sizeof fr, &other);
    node_packet(PEER, ME, MT_PORT_PRIVATE, fr, fl);
    CHECK(console_len == 0);
    /* Обычный текст Meshtastic */
    node_packet(PEER, MT_BROADCAST, MT_PORT_TEXT, (const uint8_t *)"hello mesh", 10);
    CHECK(strstr(console, "[mesh] peer: hello mesh") != NULL);

    /* Сердцебиение раз в минуту */
    now_ms += 61000;
    app_poll();
    k = pull_toradio(tr, 16);
    CHECK(k == 1 && tr[0].heartbeat);
}

int main(void) {
    test_pb();
    test_parser();
    test_smesh();
    test_app();
    printf("%s: %d проверок, провалено %d\n", fails ? "ОШИБКИ" : "ок", checks, fails);
    return fails != 0;
}
