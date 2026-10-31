/*
 * Serial API Meshtastic: кадры 0x94 0xC3 <длина BE16> <protobuf>.
 *
 * Узел Meshtastic подключается по UART; клиент шлёт ему ToRadio и получает
 * FromRadio. Номера полей сверены с meshtastic/protobuf (mesh.proto),
 * проверка — tests/interop.py против эталонной библиотеки.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SMESS_MT_H
#define SMESS_MT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MT_START1 0x94
#define MT_START2 0xC3
#define MT_MAX_PAYLOAD 512         /* больше прошивка не принимает */
#define MT_DATA_PAYLOAD_LEN 233    /* Constants.DATA_PAYLOAD_LEN */
#define MT_BROADCAST 0xFFFFFFFFu

#define MT_PORT_TEXT 1             /* TEXT_MESSAGE_APP — обычный текст Meshtastic */
#define MT_PORT_PRIVATE 256        /* PRIVATE_APP — наши конверты */

typedef struct {
    uint32_t from, to, id, rx_time;
    uint32_t channel, hop_limit, hop_start;
    bool want_ack;
    bool encrypted;     /* пришёл зашифрованным: чужой канал, не наш ключ */
    bool has_decoded;
    float rx_snr;
    int32_t rx_rssi;
    uint32_t portnum;
    uint32_t request_id;
    const uint8_t *payload; /* указывает в буфер кадра — копировать до следующего */
    size_t payload_len;
} mt_packet;

typedef enum {
    MT_FR_OTHER = 0,
    MT_FR_PACKET,
    MT_FR_MY_INFO,
    MT_FR_NODE_INFO,
    MT_FR_CONFIG_COMPLETE,
    MT_FR_REBOOTED,
    MT_FR_QUEUE_STATUS,
} mt_fr_kind;

typedef struct {
    uint32_t num;
    char id[12];
    char long_name[40];
    char short_name[8];
    float snr;
    uint32_t last_heard;
    uint32_t hops_away;
} mt_node;

typedef struct {
    mt_fr_kind kind;
    uint32_t id;
    mt_packet packet;
    uint32_t my_node_num;
    mt_node node;
    uint32_t config_complete_id;
    struct {
        int32_t res;
        uint32_t free, maxlen, mesh_packet_id;
    } queue;
} mt_fromradio;

bool mt_decode_fromradio(const uint8_t *buf, size_t len, mt_fromradio *out);

/* Отправка пакета в сеть. */
typedef struct {
    uint32_t to;          /* номер узла или MT_BROADCAST */
    uint32_t channel;     /* индекс канала на узле, 0 — основной */
    uint32_t portnum;
    const uint8_t *payload;
    size_t len;           /* не больше MT_DATA_PAYLOAD_LEN */
    uint32_t id;          /* ненулевой, уникальный — по нему узел шлёт подтверждение */
    uint32_t hop_limit;   /* 0 — по настройке узла */
    bool want_ack;
} mt_send;

/* Готовые кадры с заголовком. Возвращают длину или 0, если не влезло. */
size_t mt_frame_want_config(uint8_t *out, size_t cap, uint32_t config_id);
size_t mt_frame_heartbeat(uint8_t *out, size_t cap, uint32_t nonce);
size_t mt_frame_send(uint8_t *out, size_t cap, const mt_send *s);

/* Пробуждение: узел спит на UART и теряет первый кадр без этого. */
#define MT_WAKE_LEN 32
void mt_wake_bytes(uint8_t out[MT_WAKE_LEN]);

/*
 * Разбор потока по байту — годится для прерывания UART.
 * Байты вне кадров — текстовый журнал прошивки, уходят в on_log.
 */
typedef struct {
    uint8_t state;
    uint16_t need, got;
    void (*on_log)(uint8_t b, void *ctx);
    void *ctx;
    uint8_t buf[MT_MAX_PAYLOAD];
} mt_parser;

void mt_parser_init(mt_parser *p, void (*on_log)(uint8_t, void *), void *ctx);
/* > 0 — в p->buf лежит целый кадр такой длины. */
int mt_parser_feed(mt_parser *p, uint8_t b);

#endif
