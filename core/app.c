/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mt.h"
#include "smesh.h"
#include "smesh_crypto.h"

#ifndef APP_MAX_NODES
#define APP_MAX_NODES 32
#endif
#define WAKE_SETTLE_MS 150u      /* узел просыпается после байтов пробуждения */
#define CONFIG_RETRY_MS 8000u
#define HEARTBEAT_MS 60000u
/* Пауза между кусками одного сообщения. Кадр на LongFast идёт в эфире
 * около секунды; без паузы куски копятся в очереди узла (16 мест). */
#define FRAG_GAP_MS 1500u

static const smess_platform *P;
static mt_parser parser;
static uint8_t tx[MT_MAX_PAYLOAD + 4];

static enum { ST_WAKE, ST_CONFIG, ST_READY } state;
static uint32_t t_state, config_id, my_num, t_heartbeat;
static uint32_t channel;
static uint32_t dest = MT_BROADCAST;

static mt_node nodes[APP_MAX_NODES];
static unsigned n_nodes;

static smesh_rx rx;
static uint8_t rx_msg[SMESH_MAX_MSG];
static uint8_t rx_plain[SMESH_MAX_MSG];

static struct {
    bool busy;
    uint32_t to_node, msg_id, t_next;
    smesh_addr to;
    uint8_t total, next;
    size_t len;
    uint8_t body[SMESH_MAX_MSG];
} out;

/* Строка ввода вмещает целое сообщение: раньше было 256, и длинное
 * письмо молча обрезалось до первых 255 байт. */
static char line[SMESH_MAX_MSG + 1];
static size_t line_len;
static bool last_cr, line_overflow;

static void say(const char *fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    P->console_write(buf);
}

static bool due(uint32_t now, uint32_t t) {
    return (int32_t)(now - t) >= 0;
}

static uint32_t new_id(void) {
    uint32_t v;
    do v = P->random32(); while (!v);
    return v;
}

/* --- узлы --- */

static mt_node *node_find(uint32_t num) {
    for (unsigned i = 0; i < n_nodes; i++)
        if (nodes[i].num == num) return &nodes[i];
    return NULL;
}

static void node_upsert(const mt_node *n) {
    mt_node *slot = node_find(n->num);
    if (!slot && n_nodes < APP_MAX_NODES) slot = &nodes[n_nodes++];
    if (!slot) {
        /* Мест нет — вытесняем того, кого слышали давнее всех. Свой узел
         * не трогаем никогда: у него last_heard = 0, и раньше он вылетал
         * первым — на живой сети в 200 узлов клиент забывал своё имя. */
        for (unsigned i = 0; i < n_nodes; i++)
            if (nodes[i].num != my_num && (!slot || nodes[i].last_heard < slot->last_heard)) slot = &nodes[i];
        if (!slot) return;
        if (n->num != my_num && slot->last_heard > n->last_heard) return;
    }
    *slot = *n;
}

static const char *node_name(uint32_t num, char buf[16]) {
    const mt_node *n = node_find(num);
    if (n && n->short_name[0]) return n->short_name;
    snprintf(buf, 16, "!%08lx", (unsigned long)num);
    return buf;
}

/* --- радио --- */

static bool radio_send(uint32_t to, uint32_t port, const uint8_t *d, size_t n) {
    mt_send s = { 0 };
    s.to = to;
    s.channel = channel;
    s.portnum = port;
    s.payload = d;
    s.len = n;
    s.id = new_id();
    s.want_ack = to != MT_BROADCAST;
    size_t len = mt_frame_send(tx, sizeof tx, &s);
    if (!len) return false;
    P->radio_write(tx, len);
    return true;
}

static void wake(void) {
    uint8_t w[MT_WAKE_LEN];
    mt_wake_bytes(w);
    P->radio_write(w, sizeof w);
    state = ST_WAKE;
    t_state = P->millis();
}

static void request_config(void) {
    config_id = new_id();
    size_t n = mt_frame_want_config(tx, sizeof tx, config_id);
    P->radio_write(tx, n);
    state = ST_CONFIG;
    t_state = P->millis();
}

static void on_packet(const mt_packet *p) {
    char nb[16];
    if (!p->has_decoded) return;

    if (p->portnum == MT_PORT_TEXT) {
        say("[mesh] %s%s: %.*s\n", node_name(p->from, nb), p->to == MT_BROADCAST ? "" : " (лично)",
            (int)p->payload_len, p->payload ? (const char *)p->payload : "");
        return;
    }
    if (p->portnum != MT_PORT_PRIVATE || !p->payload) return;

    smesh_frame f;
    if (!smesh_decode(p->payload, p->payload_len, &f)) return;
    smesh_addr self = smesh_crypto_self();
    bool to_all = smesh_addr_eq(&f.to, &SMESH_ALL);
    if (!to_all && !smesh_addr_eq(&f.to, &self)) return;

    int n = smesh_rx_push(&rx, &f, P->millis(), rx_msg, sizeof rx_msg);
    if (n <= 0) return;
    long m = smesh_open(&f.from, &f.to, rx_msg, (size_t)n, rx_plain, sizeof rx_plain);
    if (m < 0) {
        say("<- %s: [не открылось]\n", node_name(p->from, nb));
        return;
    }
    say("<- %s%s: %.*s\n", node_name(p->from, nb), to_all ? " (всем)" : "", (int)m, (const char *)rx_plain);
}

static void on_frame(const uint8_t *buf, size_t len) {
    mt_fromradio fr;
    if (!mt_decode_fromradio(buf, len, &fr)) return;
    switch (fr.kind) {
    case MT_FR_MY_INFO:
        my_num = fr.my_node_num;
        smesh_crypto_init(my_num);
        break;
    case MT_FR_NODE_INFO:
        node_upsert(&fr.node);
        break;
    case MT_FR_CONFIG_COMPLETE:
        if (fr.config_complete_id != config_id || state == ST_READY) break;
        state = ST_READY;
        t_heartbeat = P->millis();
        {
            char nb[16];
            say("Готов: свой узел !%08lx (%s), в базе узлов %u. /help — команды\n",
                (unsigned long)my_num, node_name(my_num, nb), n_nodes);
        }
        break;
    case MT_FR_REBOOTED:
        say("Узел перезагрузился — подключаюсь заново\n");
        n_nodes = 0;
        wake();
        break;
    case MT_FR_PACKET:
        on_packet(&fr.packet);
        break;
    default:
        break;
    }
}

static void on_radio_log(uint8_t b, void *ctx) {
    (void)ctx;
    if (P->radio_log) P->radio_log(b);
}

/* --- отправка --- */

static void send_text(const char *text) {
    if (state != ST_READY) {
        say("Узел ещё не готов\n");
        return;
    }
    if (out.busy) {
        say("Предыдущее ещё уходит (%u из %u)\n", out.next, out.total);
        return;
    }
    out.to = dest == MT_BROADCAST ? SMESH_ALL : smesh_crypto_peer(dest);
    size_t len = smesh_seal(&out.to, (const uint8_t *)text, strlen(text), out.body, sizeof out.body);
    unsigned total = smesh_frag_count(len);
    if (!len || !total) {
        say("Слишком длинное: не больше %u байт\n",
            (unsigned)(SMESH_MAX_MSG - smesh_crypto_overhead()));
        return;
    }
    out.busy = true;
    out.to_node = dest;
    out.msg_id = new_id();
    out.total = (uint8_t)total;
    out.next = 0;
    out.len = len;
    out.t_next = P->millis();
}

static void pump_out(uint32_t now) {
    if (!out.busy || state != ST_READY || !due(now, out.t_next)) return;
    size_t off = (size_t)out.next * SMESH_FRAG_BODY;
    size_t blen = out.len - off < SMESH_FRAG_BODY ? out.len - off : SMESH_FRAG_BODY;
    smesh_frame f = { SMESH_T_TEXT, out.msg_id, smesh_crypto_self(), out.to, out.next, out.total,
                      out.body + off, blen };
    uint8_t frame[MT_DATA_PAYLOAD_LEN];
    size_t fl = smesh_encode(frame, sizeof frame, &f);
    if (!fl || !radio_send(out.to_node, MT_PORT_PRIVATE, frame, fl)) {
        say("Не удалось собрать кадр — сообщение отменено\n");
        out.busy = false;
        return;
    }
    out.next++;
    out.t_next = now + FRAG_GAP_MS;
    if (out.next == out.total) {
        out.busy = false;
        char nb[16];
        say("-> %s: отправлено (%u кадр.)\n",
            out.to_node == MT_BROADCAST ? "всем" : node_name(out.to_node, nb), out.total);
    }
}

/* --- команды --- */

static bool parse_dest(const char *arg, uint32_t *res) {
    char *end;
    if (!strcmp(arg, "all") || !strcmp(arg, "все")) {
        *res = MT_BROADCAST;
        return true;
    }
    if (arg[0] == '!') {
        unsigned long v = strtoul(arg + 1, &end, 16);
        if (end != arg + 1 && !*end) {
            *res = (uint32_t)v;
            return true;
        }
        return false;
    }
    for (unsigned i = 0; i < n_nodes; i++)
        if (!strcmp(arg, nodes[i].short_name) || !strcmp(arg, nodes[i].long_name)) {
            *res = nodes[i].num;
            return true;
        }
    unsigned long v = strtoul(arg, &end, 10);
    if (end != arg && !*end && v) {
        *res = (uint32_t)v;
        return true;
    }
    return false;
}

static void cmd_nodes(void) {
    say("Узлов: %u\n", n_nodes);
    for (unsigned i = 0; i < n_nodes; i++) {
        const mt_node *n = &nodes[i];
        say("  !%08lx  %-5s %-24s snr %d, хопов %lu%s\n", (unsigned long)n->num, n->short_name,
            n->long_name, (int)n->snr, (unsigned long)n->hops_away, n->num == my_num ? "  ← это я" : "");
    }
}

static void cmd_info(void) {
    char nb[16];
    say("Свой узел: !%08lx (%s)\n", (unsigned long)my_num, node_name(my_num, nb));
    say("Состояние: %s, канал %lu, адресат %s\n",
        state == ST_READY ? "на связи" : "подключаюсь", (unsigned long)channel,
        dest == MT_BROADCAST ? "все" : node_name(dest, nb));
    say("Шифрование содержимого: %s\n", smesh_crypto_name());
    say("Сообщение: до %u байт (%u кадр. по %u)\n", (unsigned)(SMESH_MAX_MSG - smesh_crypto_overhead()),
        SMESH_MAX_FRAGS, SMESH_FRAG_BODY);
}

static void process_line(char *s) {
    while (*s == ' ') s++;
    if (!*s) return;
    if (*s != '/') {
        send_text(s);
        return;
    }
    char *arg = strchr(s, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ') arg++;
    }
    if (!strcmp(s, "/help")) {
        say("Текст без / — сообщение STORAGE&MESS текущему адресату.\n"
            "  /to <all|!номер|имя>  адресат\n"
            "  /mesh <текст>         обычное сообщение Meshtastic (видно в их приложениях)\n"
            "  /nodes                узлы в сети\n"
            "  /ch <номер>           канал узла (0 — основной)\n"
            "  /info                 состояние\n");
    } else if (!strcmp(s, "/nodes")) {
        cmd_nodes();
    } else if (!strcmp(s, "/info")) {
        cmd_info();
    } else if (!strcmp(s, "/to")) {
        uint32_t d;
        char nb[16];
        if (arg && *arg && parse_dest(arg, &d)) {
            dest = d;
            say("Адресат: %s\n", d == MT_BROADCAST ? "все" : node_name(d, nb));
        } else {
            say("Не понял адресата. Пример: /to !a1b2c3d4, /to all\n");
        }
    } else if (!strcmp(s, "/ch")) {
        if (arg && *arg) channel = (uint32_t)strtoul(arg, NULL, 10);
        say("Канал: %lu\n", (unsigned long)channel);
    } else if (!strcmp(s, "/mesh")) {
        if (state != ST_READY) say("Узел ещё не готов\n");
        else if (!arg || !*arg) say("Пустое сообщение\n");
        else if (strlen(arg) > MT_DATA_PAYLOAD_LEN) say("Не больше %u байт\n", MT_DATA_PAYLOAD_LEN);
        else if (radio_send(dest, MT_PORT_TEXT, (const uint8_t *)arg, strlen(arg))) say("-> [mesh] отправлено\n");
    } else {
        say("Нет такой команды. /help\n");
    }
}

/* --- снаружи --- */

void app_init(const smess_platform *p) {
    P = p;
    mt_parser_init(&parser, on_radio_log, NULL);
    smesh_rx_init(&rx);
    memset(&out, 0, sizeof out);
    n_nodes = 0;
    line_len = 0;
    say("STORAGE&MESS LoRa — подключаюсь к узлу Meshtastic…\n");
    wake();
}

void app_radio_byte(uint8_t b) {
    int n = mt_parser_feed(&parser, b);
    if (n > 0) on_frame(parser.buf, (size_t)n);
}

void app_console_char(char c) {
    if (c == '\n' && last_cr) {
        last_cr = false;
        return;
    }
    last_cr = c == '\r';
    if (c == '\r' || c == '\n') {
        if (P->echo) P->console_write("\r\n");
        line[line_len] = 0;
        line_len = 0;
        if (line_overflow) say("Строка длиннее %u байт — хвост отброшен\n", (unsigned)(sizeof line - 1));
        line_overflow = false;
        process_line(line);
        return;
    }
    if (c == 0x7F || c == 0x08) {
        if (line_len) {
            line_len--;
            if (P->echo) P->console_write("\b \b");
        }
        return;
    }
    if (line_len + 1 >= sizeof line) {
        line_overflow = true;
        return;
    }
    {
        line[line_len++] = c;
        if (P->echo) {
            char e[2] = { c, 0 };
            P->console_write(e);
        }
    }
}

void app_poll(void) {
    uint32_t now = P->millis();
    if (state == ST_WAKE && due(now, t_state + WAKE_SETTLE_MS)) request_config();
    if (state == ST_CONFIG && due(now, t_state + CONFIG_RETRY_MS)) wake();
    if (state == ST_READY && due(now, t_heartbeat + HEARTBEAT_MS)) {
        size_t n = mt_frame_heartbeat(tx, sizeof tx, new_id());
        P->radio_write(tx, n);
        t_heartbeat = now;
    }
    pump_out(now);
}

bool app_ready(void) {
    return state == ST_READY;
}
