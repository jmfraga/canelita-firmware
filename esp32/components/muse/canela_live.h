/*
 * Copyright (c) 2026 Juan Manuel Fraga.
 * SPDX-License-Identifier: Apache-2.0
 *
 * canelita: llamada con Canela por gpt-live-1 sobre WebSocket (canela_live.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CANELA_LIVE_APAGADA,
    CANELA_LIVE_CONECTANDO,
    CANELA_LIVE_LISTA,       /* en llamada */
    CANELA_LIVE_PENSANDO,    /* Canela consulta sus herramientas */
    CANELA_LIVE_FIN,         /* el servidor colgó (tiempo, cupo, cierre) */
    CANELA_LIVE_ERROR,
} canela_live_estado_t;

/* Abre la llamada (no bloquea); el estado pasa a LISTA cuando el brain la acepta. */
bool canela_live_start(const char *url_base, const char *cf_id, const char *cf_secret, const char *token);
canela_live_estado_t canela_live_estado(void);
const char *canela_live_error(void);
int canela_live_max_s(void);
/* PENSANDO → LISTA cuando vuelve a sonar Canela. */
void canela_live_seguir(void);
/* Micrófono → servidor (PCM16 16 kHz). */
bool canela_live_send(const int16_t *pcm, size_t frames);
/* Voz de Canela → bocina; 0 si no hay nada pendiente. */
size_t canela_live_read(int16_t *pcm, size_t frames);
void canela_live_stop(void);

/* De canela_chat.c: abre la llamada con la configuración guardada (NVS). */
bool canela_live_abrir(void);
