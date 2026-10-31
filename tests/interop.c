/*
 * Мост для tests/interop.py: наш кодек против эталонной библиотеки
 * Meshtastic. enc — печатает наши кадры, dec <hex> — разбирает FromRadio.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mt.h"

static void hex(const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", d[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    uint8_t out[600];
    if (argc == 2 && !strcmp(argv[1], "enc")) {
        hex(out, mt_frame_want_config(out, sizeof out, 0x1234567));
        hex(out, mt_frame_heartbeat(out, sizeof out, 777));
        const char *t = "привет, mesh";
        mt_send s = { 0x12345678, 1, MT_PORT_PRIVATE, (const uint8_t *)t, strlen(t), 0xABCDEF01, 3, true };
        hex(out, mt_frame_send(out, sizeof out, &s));
        uint8_t big[MT_DATA_PAYLOAD_LEN];
        for (int i = 0; i < MT_DATA_PAYLOAD_LEN; i++) big[i] = (uint8_t)i;
        mt_send b = { MT_BROADCAST, 0, MT_PORT_TEXT, big, sizeof big, 1, 0, false };
        hex(out, mt_frame_send(out, sizeof out, &b));
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "dec")) {
        size_t n = strlen(argv[2]) / 2;
        uint8_t *b = malloc(n + 1);
        for (size_t i = 0; i < n; i++) sscanf(argv[2] + 2 * i, "%2hhx", &b[i]);
        mt_fromradio fr;
        bool ok = mt_decode_fromradio(b, n, &fr);
        printf("ok=%d kind=%d frid=%u\n", ok, fr.kind, fr.id);
        const mt_packet *p = &fr.packet;
        printf("from=%u to=%u channel=%u pid=%u hop_limit=%u hop_start=%u want_ack=%d rx_snr=%.2f rx_rssi=%d "
               "rx_time=%u encrypted=%d decoded=%d portnum=%u request_id=%u payload=",
               p->from, p->to, p->channel, p->id, p->hop_limit, p->hop_start, p->want_ack, p->rx_snr,
               p->rx_rssi, p->rx_time, p->encrypted, p->has_decoded, p->portnum, p->request_id);
        hex(p->payload ? p->payload : (const uint8_t *)"", p->payload_len);
        printf("my_node_num=%u\n", fr.my_node_num);
        printf("node num=%u nid=%s long=%s short=%s snr=%.2f last_heard=%u hops=%u\n", fr.node.num, fr.node.id,
               fr.node.long_name, fr.node.short_name, fr.node.snr, fr.node.last_heard, fr.node.hops_away);
        printf("config_complete_id=%u\n", fr.config_complete_id);
        printf("queue res=%d free=%u maxlen=%u id=%u\n", fr.queue.res, fr.queue.free, fr.queue.maxlen,
               fr.queue.mesh_packet_id);
        free(b);
        return ok ? 0 : 1;
    }
    fprintf(stderr, "usage: interop enc | interop dec <hex>\n");
    return 2;
}
