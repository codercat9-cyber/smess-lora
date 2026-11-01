/*
 * Защита содержимого сообщений — точка подключения.
 *
 * Сами механизмы (ключи, обмен, шифр, подписи) — закрытая часть
 * STORAGE&MESS и в открытый код не входят. Здесь только интерфейс; в
 * открытой сборке стоит crypto_null.c: тело идёт как есть, а в эфире его
 * закрывает лишь ключ канала Meshtastic. Закрытая реализация собирается
 * так: make CRYPTO=private (берётся private/, в репозиторий не попадает).
 *
 * Модулю принадлежат служебные кадры (SMESH_T_CTRL) и все типы с битом
 * SMESH_T_SEALED: открытая сборка их не понимает и честно об этом пишет.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SMESS_SMESH_CRYPTO_H
#define SMESS_SMESH_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "smesh.h"

/* Что платформа даёт модулю. Любое поле может быть NULL. */
typedef struct {
    /* Случайные байты из лучшего источника платформы. */
    void (*entropy)(uint8_t *buf, size_t n);
    /* Небольшое постоянное хранилище секрета (флеш, файл с правами 600). */
    bool (*secret_load)(uint8_t *buf, size_t n);
    bool (*secret_store)(const uint8_t *buf, size_t n);
    /* Сообщить человеку (заполняет ядро, платформе трогать не нужно). */
    void (*notice)(const char *line);
} smesh_crypto_platform;

/* Насколько можно верить пришедшему. */
typedef enum {
    SMESH_TRUST_NONE = 0,  /* как есть: кто угодно мог написать от этого имени */
    SMESH_TRUST_SIGNED,    /* отправитель подтверждён */
    SMESH_TRUST_PRIVATE,   /* подтверждён и прочесть мог только адресат */
} smesh_trust;

/* Вызывается при старте и при каждой смене своего узла Meshtastic. */
void smesh_crypto_init(const smesh_crypto_platform *p, uint32_t node_num);

/* Подмешать непредсказуемое (время прихода байта и т. п.). Дёшево. */
void smesh_crypto_stir(uint32_t v);

/* Свой адрес в конверте. */
smesh_addr smesh_crypto_self(void);

/* Адрес собеседника по номеру его узла. false — писать ему лично пока
 * нельзя (модуль сам попросит его представиться). */
bool smesh_crypto_peer(uint32_t node_num, smesh_addr *out);

/* Сколько байт добавляет запечатывание: лично и всем. */
size_t smesh_crypto_overhead(bool to_all);

/* Запечатать тело. В hdr заполнены тип, id и адреса; тип кадра в эфире
 * модуль пишет обратно в hdr->type. Возвращает длину или 0. */
size_t smesh_seal(smesh_frame *hdr, const uint8_t *in, size_t n, uint8_t *out, size_t cap);

/* Открыть собранное тело из кадра hdr от узла from_node. В hdr->type
 * вернётся тип содержимого. -1 — не открывается (чужое, испорчено). */
long smesh_open(uint32_t from_node, smesh_frame *hdr, const uint8_t *in, size_t n,
                uint8_t *out, size_t cap, smesh_trust *trust);

/* Служебный кадр модуля (SMESH_T_CTRL) пришёл от узла from_node. */
void smesh_crypto_on_ctrl(uint32_t from_node, const smesh_frame *hdr, const uint8_t *body, size_t n);

/* Модулю есть что сказать в эфир? Возвращает длину тела служебного кадра
 * (0 — нечего) и кому: номер узла Meshtastic и адрес в конверте. */
size_t smesh_crypto_ctrl_out(uint32_t now_ms, uint32_t *to_node, smesh_addr *to, uint8_t *body, size_t cap);

/* Команда консоли модуля (строка без '/'). true — модуль её обработал. */
bool smesh_crypto_command(const char *cmd, const char *arg, void (*out)(const char *line));

/* Для /info: имя механизма и свой отпечаток (пусто, если ключей нет). */
const char *smesh_crypto_name(void);
const char *smesh_crypto_id(void);

#endif
