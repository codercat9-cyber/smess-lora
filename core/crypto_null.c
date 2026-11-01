/*
 * Открытая заглушка: без защиты содержимого. См. smesh_crypto.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>

#include "smesh_crypto.h"

static smesh_addr self;

void smesh_crypto_init(const smesh_crypto_platform *p, uint32_t node_num) {
    (void)p;
    self = smesh_addr_from_node(node_num);
}

void smesh_crypto_stir(uint32_t v) {
    (void)v;
}

smesh_addr smesh_crypto_self(void) {
    return self;
}

bool smesh_crypto_peer(uint32_t node_num, smesh_addr *out) {
    *out = smesh_addr_from_node(node_num);
    return true;
}

size_t smesh_crypto_overhead(bool to_all) {
    (void)to_all;
    return 0;
}

size_t smesh_seal(smesh_frame *hdr, const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    (void)hdr;
    if (n > cap) return 0;
    memcpy(out, in, n);
    return n;
}

long smesh_open(uint32_t from_node, smesh_frame *hdr, const uint8_t *in, size_t n,
                uint8_t *out, size_t cap, smesh_trust *trust) {
    (void)from_node;
    *trust = SMESH_TRUST_NONE;
    if ((hdr->type & SMESH_T_SEALED) || n > cap) return -1;
    memcpy(out, in, n);
    return (long)n;
}

void smesh_crypto_on_ctrl(uint32_t from_node, const smesh_frame *hdr, const uint8_t *body, size_t n) {
    (void)from_node;
    (void)hdr;
    (void)body;
    (void)n;
}

size_t smesh_crypto_ctrl_out(uint32_t now_ms, uint32_t *to_node, smesh_addr *to, uint8_t *body, size_t cap) {
    (void)now_ms;
    (void)to_node;
    (void)to;
    (void)body;
    (void)cap;
    return 0;
}

bool smesh_crypto_command(const char *cmd, const char *arg, void (*out)(const char *line)) {
    (void)cmd;
    (void)arg;
    (void)out;
    return false;
}

const char *smesh_crypto_name(void) {
    return "нет (открытая сборка)";
}

const char *smesh_crypto_id(void) {
    return "";
}
