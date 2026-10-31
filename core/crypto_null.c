/*
 * Открытая заглушка: без шифрования содержимого. См. smesh_crypto.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>

#include "smesh_crypto.h"

static smesh_addr self;

void smesh_crypto_init(uint32_t node_num) {
    self = smesh_addr_from_node(node_num);
}

smesh_addr smesh_crypto_self(void) {
    return self;
}

smesh_addr smesh_crypto_peer(uint32_t node_num) {
    return smesh_addr_from_node(node_num);
}

size_t smesh_crypto_overhead(void) {
    return 0;
}

size_t smesh_seal(const smesh_addr *to, const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    (void)to;
    if (n > cap) return 0;
    memcpy(out, in, n);
    return n;
}

long smesh_open(const smesh_addr *from, const smesh_addr *to, const uint8_t *in, size_t n,
                uint8_t *out, size_t cap) {
    (void)from;
    (void)to;
    if (n > cap) return -1;
    memcpy(out, in, n);
    return (long)n;
}

const char *smesh_crypto_name(void) {
    return "нет (открытая сборка)";
}
