/*
 * Copyright (c) 2026 Juan Manuel Fraga.
 * SPDX-License-Identifier: Apache-2.0
 *
 * canelita: llamada con Canela por gpt-live-1 (un toque corto al botón de hablar).
 *
 * La placa no tiene WebRTC: abre un WebSocket a la puerta de voz
 * (canela.docfraga.com, /v1/agentes/canela/live) con los mismos tres secretos
 * que canela_chat.c. canela-web lo empalma con el brain, que habla con
 * gpt-live-1 (la llave de OpenAI nunca llega aquí). Binario en los dos
 * sentidos = PCM16 mono 16 kHz; texto = eventos JSON pequeños del brain
 * ({"type":"estado"|"oido"|"fin"|"error", ...}).
 *
 * La voz de Canela llega más rápido que tiempo real: se guarda en un anillo en
 * PSRAM y muse_voice la saca a la bocina a su paso (canela_live_read).
 */
#include "canela_live.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "canela_live";

#define RING_FRAMES (16000 * 60)            /* 60 s de voz de Canela en PSRAM (~1.9 MB) */
#define MIC_FRAMES  (16000 * 4)             /* 4 s de micrófono por subir */
#define MIC_ENVIO   (16000 / 10)            /* se sube en trozos de 100 ms */
#define TEXT_MAX    512
#define BIG (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t s_lock;
static int16_t *s_ring;
static size_t s_rd, s_wr, s_fill;           /* en muestras */
static uint8_t s_odd[1];                     /* medio sample de un trozo binario partido */
static bool s_has_odd;
static char s_text[TEXT_MAX];                /* un evento de texto que llega en pedazos */
static size_t s_text_len;
static int s_op;                            /* tipo del mensaje en curso (1 texto, 2 binario) */
static volatile canela_live_estado_t s_estado = CANELA_LIVE_APAGADA;
static char s_error[64];
static volatile int s_max_s;
/* El micrófono no se sube desde el lazo de audio: el cliente WebSocket toma un candado mientras recibe la voz
 * de Canela y un envío podía esperar 200 ms (se perdían trozos y se trababa el audio). Una tarea aparte sube. */
static int16_t *s_mic;
static size_t s_mrd, s_mwr, s_mfill;
static TaskHandle_t s_subida;
static volatile bool s_subir;
static void subida(void *arg);

/* Saca el valor de un campo string de un JSON pequeño y plano ("clave":"valor"). */
static bool campo(const char *json, const char *clave, char *out, size_t cap)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", clave);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    while (*++p == ' ') {}
    if (*p != '"') {                         /* número */
        snprintf(out, cap, "%.*s", (int)strcspn(p, ",}"), p);
        return true;
    }
    p++;
    size_t n = strcspn(p, "\"");
    snprintf(out, cap, "%.*s", (int)(n < cap - 1 ? n : cap - 1), p);
    return true;
}

static void evento_texto(const char *json)
{
    char tipo[16] = "", v[64] = "";
    campo(json, "type", tipo, sizeof(tipo));
    if (!strcmp(tipo, "estado")) {
        campo(json, "estado", v, sizeof(v));
        if (!strcmp(v, "lista")) {
            char m[16] = "";
            if (campo(json, "max_s", m, sizeof(m))) s_max_s = atoi(m);
            s_estado = CANELA_LIVE_LISTA;
        } else if (!strcmp(v, "pensando")) {
            s_estado = CANELA_LIVE_PENSANDO;
        }
    } else if (!strcmp(tipo, "fin")) {
        campo(json, "razon", v, sizeof(v));
        ESP_LOGI(TAG, "fin: %s", v);
        s_estado = CANELA_LIVE_FIN;
    } else if (!strcmp(tipo, "error")) {
        campo(json, "texto", s_error, sizeof(s_error));
        ESP_LOGW(TAG, "error: %s", s_error);
        s_estado = CANELA_LIVE_ERROR;
    }
}

static void meter_audio(const uint8_t *p, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_has_odd && n) {                    /* completa el sample partido */
        uint8_t par[2] = { s_odd[0], p[0] };
        int16_t s;
        memcpy(&s, par, 2);
        if (s_fill < RING_FRAMES) { s_ring[s_wr] = s; s_wr = (s_wr + 1) % RING_FRAMES; s_fill++; }
        p++; n--; s_has_odd = false;
    }
    for (; n >= 2; p += 2, n -= 2) {
        int16_t s;
        memcpy(&s, p, 2);
        if (s_fill == RING_FRAMES) break;    /* lleno: se descarta (60 s de ventaja no pasa) */
        s_ring[s_wr] = s;
        s_wr = (s_wr + 1) % RING_FRAMES;
        s_fill++;
    }
    if (n == 1) { s_odd[0] = p[0]; s_has_odd = true; }
    xSemaphoreGive(s_lock);
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "conectada");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 || d->op_code == 0x2) s_op = d->op_code;   /* 0x0 = continuación */
        if (d->op_code == 0x8) {             /* cierre del servidor */
            if (s_estado != CANELA_LIVE_ERROR) s_estado = CANELA_LIVE_FIN;
        } else if (d->op_code > 0x2) {
            /* ping/pong: nada */
        } else if (s_op == 0x2) {
            meter_audio((const uint8_t *)d->data_ptr, d->data_len);
        } else {
            if (d->payload_offset == 0) s_text_len = 0;
            size_t n = d->data_len;
            if (s_text_len + n >= TEXT_MAX) n = TEXT_MAX - 1 - s_text_len;
            memcpy(s_text + s_text_len, d->data_ptr, n);
            s_text_len += n;
            if (d->payload_offset + d->data_len >= d->payload_len) {
                s_text[s_text_len] = 0;
                evento_texto(s_text);
                s_text_len = 0;
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_estado != CANELA_LIVE_ERROR) s_estado = CANELA_LIVE_FIN;
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (s_estado == CANELA_LIVE_CONECTANDO) {
            strlcpy(s_error, "NO PUDE CONECTAR", sizeof(s_error));
            s_estado = CANELA_LIVE_ERROR;
        }
        break;
    default:
        break;
    }
}

bool canela_live_start(const char *url_base, const char *cf_id, const char *cf_secret, const char *token,
                       const char *agente)
{
    canela_live_stop();
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_ring) s_ring = heap_caps_malloc(RING_FRAMES * sizeof(int16_t), BIG);
    if (!s_mic) s_mic = heap_caps_malloc(MIC_FRAMES * sizeof(int16_t), BIG);
    if (!s_lock || !s_ring || !s_mic) {
        strlcpy(s_error, "SIN MEMORIA", sizeof(s_error));
        s_estado = CANELA_LIVE_ERROR;
        return false;
    }
    s_rd = s_wr = s_fill = 0;
    s_mrd = s_mwr = s_mfill = 0;
    s_has_odd = false;
    s_text_len = 0;
    s_error[0] = 0;
    s_max_s = 0;
    s_estado = CANELA_LIVE_CONECTANDO;

    /* https://host → wss://host */
    char uri[200];
    const char *host = strstr(url_base, "://");
    snprintf(uri, sizeof(uri), "wss://%s/v1/agentes/%s/live", host ? host + 3 : url_base, agente);
    static char headers[512];
    snprintf(headers, sizeof(headers),
             "CF-Access-Client-Id: %s\r\nCF-Access-Client-Secret: %s\r\nAuthorization: Bearer %s\r\n",
             cf_id, cf_secret, token);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .network_timeout_ms = 15000,
        .reconnect_timeout_ms = 0,
        .disable_auto_reconnect = true,
        .task_stack = 8192,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        strlcpy(s_error, "SIN MEMORIA", sizeof(s_error));
        s_estado = CANELA_LIVE_ERROR;
        return false;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        strlcpy(s_error, "NO PUDE CONECTAR", sizeof(s_error));
        s_estado = CANELA_LIVE_ERROR;
        return false;
    }
    s_subir = true;   /* pila en RAM interna: la tarea se borra sola al terminar */
    if (xTaskCreatePinnedToCore(subida, "live_subida", 4096, NULL, 5, &s_subida, 0) != pdPASS) {
        s_subir = false;
        strlcpy(s_error, "SIN MEMORIA", sizeof(s_error));
        s_estado = CANELA_LIVE_ERROR;
        return false;
    }
    return true;
}

canela_live_estado_t canela_live_estado(void) { return s_estado; }
const char *canela_live_error(void) { return s_error[0] ? s_error : "SE CORTO LA LLAMADA"; }
int canela_live_max_s(void) { return s_max_s; }

void canela_live_seguir(void)
{
    if (s_estado == CANELA_LIVE_PENSANDO) s_estado = CANELA_LIVE_LISTA;
}

bool canela_live_send(const int16_t *pcm, size_t frames)
{
    if (!s_mic || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < frames && s_mfill < MIC_FRAMES; i++) {
        s_mic[s_mwr] = pcm[i];
        s_mwr = (s_mwr + 1) % MIC_FRAMES;
        s_mfill++;
    }
    xSemaphoreGive(s_lock);
    if (s_subida) xTaskNotifyGive(s_subida);
    return true;
}

static void subida(void *arg)
{
    (void)arg;
    static int16_t trozo[MIC_ENVIO];
    while (s_subir) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        for (;;) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            size_t n = s_mfill >= MIC_ENVIO ? MIC_ENVIO : 0;
            for (size_t i = 0; i < n; i++) {
                trozo[i] = s_mic[s_mrd];
                s_mrd = (s_mrd + 1) % MIC_FRAMES;
            }
            s_mfill -= n;
            xSemaphoreGive(s_lock);
            if (!n || !s_subir) break;
            if (s_ws && esp_websocket_client_is_connected(s_ws)) {
                esp_websocket_client_send_bin(s_ws, (const char *)trozo, n * sizeof(int16_t), pdMS_TO_TICKS(1000));
            }
        }
    }
    s_subida = NULL;
    vTaskDelete(NULL);
}

size_t canela_live_read(int16_t *pcm, size_t frames)
{
    if (!s_lock || !s_ring) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_fill < frames ? s_fill : frames;
    for (size_t i = 0; i < n; i++) {
        pcm[i] = s_ring[s_rd];
        s_rd = (s_rd + 1) % RING_FRAMES;
    }
    s_fill -= n;
    xSemaphoreGive(s_lock);
    return n;
}

void canela_live_stop(void)
{
    s_subir = false;
    for (int i = 0; i < 40 && s_subida; i++) vTaskDelay(pdMS_TO_TICKS(50));   /* que termine su envío */
    if (!s_ws) return;
    if (esp_websocket_client_is_connected(s_ws)) {
        const char *bye = "{\"type\":\"cerrar\"}";
        esp_websocket_client_send_text(s_ws, bye, strlen(bye), pdMS_TO_TICKS(500));
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(2000));
    }
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    if (s_estado != CANELA_LIVE_ERROR) s_estado = CANELA_LIVE_APAGADA;
}
