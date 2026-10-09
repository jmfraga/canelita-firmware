/*
 * Copyright (c) 2026 Juan Manuel Fraga.
 * SPDX-License-Identifier: Apache-2.0
 *
 * canelita: el push-to-talk habla con Canela en lugar de con la nube de Muse.
 *
 * Implementa la API muse_hatch_* (muse_chat.h) contra la puerta de voz de
 * canela-web (canela.docfraga.com, POST /v1/agentes/canela/voz). Reemplaza a
 * muse_chat.c + muse_account_api.c + muse_chat_session.cpp cuando
 * CONFIG_CANELITA_CHAT está activo; muse_voice.c, la cara y la reproducción no
 * cambian: muse_voice ya sabe reproducir lo que devuelve muse_hatch_turn_read,
 * sólo que la sesión de Muse respondía con texto y nunca entregaba audio.
 *
 * Un turno: se acumula el PCM (16 kHz mono) mientras se habla; al soltar, una
 * tarea arma un WAV, lo manda por HTTPS con los tres secretos (dos de
 * Cloudflare Access y el token del dispositivo) y pide la respuesta POR FRASES
 * (stream=1): cada frase llega como MP3 propio, se decodifica con minimp3, se
 * pasa de 24 a 16 kHz y se agrega al audio del turno, así que la bocina empieza
 * con la primera frase mientras el servidor genera las demás.
 *
 * Configuración en NVS (espacio "canelita"), grabada por USB con los comandos
 * de consola canelita.* (ver canela_console). Nunca se imprime un valor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "minimp3.h"
#include "nvs.h"

#include "canela_chat.h"
#include "muse_audio.h"
#include "muse_chat.h"
#include "muse_chat_priv.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_wifi.h"

static const char *TAG = "canelita";

#define NS             "canelita"
#define AGENTE         "canela"
#define MIC_RATE       16000
#define MAX_SECS       15
#define PCM_CAP        (MIC_RATE * MAX_SECS)            /* frames */
#define TEXT_MAX       1024
#define EV_TEXT        96
#define FRAME_MAX      (2 * 1024 * 1024)                /* un cuadro del flujo (MP3 de una frase) */
#define MAX_SEG        48                               /* frases por respuesta, para los subtítulos */
#define BOUNDARY       "----canelita7d1f2a"

#define BIG (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STREAM_PART    "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"stream\"\r\n\r\n1\r\n"

/* ---- Configuración (NVS) ---- */

static char s_url[160], s_cf_id[96], s_cf_secret[128], s_token[96];

static void load_str(nvs_handle_t h, const char *k, char *out, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(h, k, out, &n) != ESP_OK) out[0] = 0;
}

static void load_config(void)
{
    nvs_handle_t h;
    s_url[0] = s_cf_id[0] = s_cf_secret[0] = s_token[0] = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    load_str(h, "url", s_url, sizeof(s_url));
    load_str(h, "cf_id", s_cf_id, sizeof(s_cf_id));
    load_str(h, "cf_secret", s_cf_secret, sizeof(s_cf_secret));
    load_str(h, "token", s_token, sizeof(s_token));
    nvs_close(h);
}

static bool configured(void)
{
    return s_url[0] && s_cf_id[0] && s_cf_secret[0] && s_token[0];
}

/* ---- Estado del turno ---- */

typedef struct { muse_hatch_ev_t type; char text[EV_TEXT]; } ev_t;

static QueueHandle_t s_events;
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_gen;          /* sube con cada turno/cancelación */

static struct {
    bool talking;
    int16_t *pcm;                        /* lo que se dijo, 16 kHz mono */
    size_t n;
    int16_t *out;                        /* la respuesta, 16 kHz mono; crece por frases */
    size_t out_n, out_rd, out_cap;
    char text[TEXT_MAX];                 /* para los subtítulos */
    /* Dónde termina cada frase: en el audio (muestras) y en `text` (bytes). */
    size_t seg_pcm[MAX_SEG], seg_txt[MAX_SEG];
    int nseg;
} s_turn;

static void emit(muse_hatch_ev_t type, const char *text)
{
    ev_t ev = { .type = type };
    strlcpy(ev.text, text ? text : "", sizeof(ev.text));
    if (s_events) xQueueSend(s_events, &ev, 0);
}

static void free_turn_locked(void)
{
    heap_caps_free(s_turn.pcm);
    heap_caps_free(s_turn.out);
    s_turn.pcm = s_turn.out = NULL;
    s_turn.n = s_turn.out_n = s_turn.out_rd = s_turn.out_cap = 0;
    s_turn.text[0] = 0;
    s_turn.nseg = 0;
    s_turn.talking = false;
}

/* ---- Subtítulos en ASCII ---- */

/* Las fuentes de la UI (Montserrat de LVGL) sólo traen ASCII: un acento o una
 * eñe se verían como un cuadro vacío. Se translitera SÓLO el texto que va a la
 * pantalla; la voz no cambia. Solución de fondo pendiente: una fuente Latin-1. */
static void ascii_es(char *s)
{
    static const struct { const char *u; const char *a; } map[] = {
        {"á","a"},{"é","e"},{"í","i"},{"ó","o"},{"ú","u"},{"ü","u"},{"ñ","n"},
        {"Á","A"},{"É","E"},{"Í","I"},{"Ó","O"},{"Ú","U"},{"Ü","U"},{"Ñ","N"},
        {"¿",""},{"¡",""},{"“","\""},{"”","\""},{"‘","'"},{"’","'"},{"—","-"},{"–","-"},{"…","..."},
    };
    char *w = s;
    for (const char *r = s; *r;) {
        if ((unsigned char)*r < 0x80) { *w++ = *r++; continue; }
        size_t i, n = 0;
        for (i = 0; i < sizeof(map) / sizeof(*map); i++) {
            n = strlen(map[i].u);
            if (!strncmp(r, map[i].u, n)) break;
        }
        if (i < sizeof(map) / sizeof(*map)) {
            for (const char *a = map[i].a; *a;) *w++ = *a++;   /* nunca más largo que el original */
            r += n;
        } else {
            r++;                                               /* otro byte no ASCII: se omite */
            while ((*r & 0xC0) == 0x80) r++;
        }
    }
    *w = 0;
}

/* ---- WAV ---- */

static void wav_header(uint8_t h[44], uint32_t frames)
{
    uint32_t data = frames * 2, riff = 36 + data, rate = MIC_RATE, br = MIC_RATE * 2;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt_len = 16; uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 16, &fmt_len, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4); memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
}

/* ---- MP3 → PCM 16 kHz ---- */

/* Decodifica y remuestrea (interpolación lineal) a MUSE_AUDIO_RATE. */
static int16_t *mp3_to_pcm(const uint8_t *mp3, size_t len, size_t *out_frames)
{
    static mp3dec_t dec;                 /* ~6 KB: fuera de la pila */
    mp3dec_init(&dec);
    size_t cap = MUSE_AUDIO_RATE * 8, n = 0;   /* una frase; crece si hace falta */
    int16_t *out = heap_caps_malloc(cap * 2, BIG);
    int16_t *frame = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2, BIG);
    if (!out || !frame) { heap_caps_free(out); heap_caps_free(frame); return NULL; }
    double pos = 0;                      /* posición fraccional en la entrada */
    int16_t prev = 0;
    size_t off = 0;
    while (off < len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, mp3 + off, len - off, frame, &info);
        if (!info.frame_bytes) break;
        off += info.frame_bytes;
        if (samples <= 0 || info.hz <= 0) continue;
        double step = (double)info.hz / MUSE_AUDIO_RATE;
        /* Mezcla a mono si viniera estéreo. */
        if (info.channels == 2) {
            for (int i = 0; i < samples; i++) frame[i] = (frame[2 * i] + frame[2 * i + 1]) / 2;
        }
        while (pos < samples) {
            int i = (int)pos;
            double f = pos - i;
            /* Entre la muestra anterior y la actual (la primera de cada cuadro
             * usa la última del cuadro previo): una muestra de retraso, inaudible. */
            int16_t a = i == 0 ? prev : frame[i - 1], b = frame[i];
            int32_t v = (int32_t)(a + (b - a) * f);
            if (n == cap) {
                size_t ncap = cap * 2;
                int16_t *g = heap_caps_realloc(out, ncap * 2, BIG);
                if (!g) break;
                out = g; cap = ncap;
            }
            out[n++] = (int16_t)v;
            pos += step;
        }
        pos -= samples;
        prev = frame[samples - 1];
    }
    heap_caps_free(frame);
    *out_frames = n;
    return out;
}

/* ---- La petición ---- */

typedef struct { int16_t *pcm; size_t n; char *texto; uint32_t gen; } job_t;

static const char *http_error(int status)
{
    switch (status) {
    case 401: return "CANELITA NO AUTORIZADA";
    case 403: return "AGENTE NO PERMITIDO";
    case 413: return "MENSAJE DEMASIADO LARGO";
    case 429: return "ESPERA UN MINUTO";
    case 502: return "CANELA NO RESPONDE";
    default:  return "ERROR DE LA PUERTA";
    }
}

static bool write_all(esp_http_client_handle_t c, const void *p, size_t n)
{
    const char *b = p;
    while (n) {
        int w = esp_http_client_write(c, b, n > 4096 ? 4096 : n);
        if (w <= 0) return false;
        b += w; n -= w;
    }
    return true;
}

/* Lee exactamente n bytes del cuerpo (que puede venir en trozos). */
static bool read_exact(esp_http_client_handle_t c, void *buf, size_t n)
{
    char *b = buf;
    while (n) {
        int r = esp_http_client_read(c, b, n);
        if (r <= 0) return false;
        b += r; n -= r;
    }
    return true;
}

/* Agrega una frase al audio del turno y anota dónde termina (audio y texto). */
static bool append_frase(uint32_t gen, const int16_t *pcm, size_t n, size_t txt_end)
{
    bool ok = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (gen == s_gen) {
        if (s_turn.out_n + n > s_turn.out_cap) {
            size_t ncap = s_turn.out_cap ? s_turn.out_cap : MUSE_AUDIO_RATE * 30;
            while (ncap < s_turn.out_n + n) ncap *= 2;
            int16_t *g = heap_caps_realloc(s_turn.out, ncap * 2, BIG);
            if (g) { s_turn.out = g; s_turn.out_cap = ncap; }
            else ok = false;
        }
        if (ok) {
            memcpy(s_turn.out + s_turn.out_n, pcm, n * 2);
            s_turn.out_n += n;
            int k = s_turn.nseg < MAX_SEG ? s_turn.nseg++ : MAX_SEG - 1;
            s_turn.seg_pcm[k] = s_turn.out_n;
            s_turn.seg_txt[k] = txt_end;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

static void procesar(job_t *job)
{
    uint8_t *frame = NULL;               /* un cuadro del flujo */
    char *reply_utf8 = NULL;             /* R tal cual: las frases dicen dónde terminan en él */
    int16_t *out = NULL;
    const char *fail = NULL;
    static char fail_buf[EV_TEXT];       /* un error que manda el servidor (cuadro E) */
    char url[200];
    snprintf(url, sizeof(url), "%s/v1/agentes/" AGENTE "/voz", s_url);

    /* Cuerpo multipart: "stream"=1 (respuesta por frases) y un campo "audio"
     * (WAV) o "texto". */
    char head[384], tail[48];
    uint8_t wav[44];
    size_t body_len;
    if (job->texto) {
        snprintf(head, sizeof(head), STREAM_PART "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"texto\"\r\n\r\n");
        body_len = strlen(head) + strlen(job->texto);
    } else {
        snprintf(head, sizeof(head), STREAM_PART "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"audio\"; "
                 "filename=\"voz.wav\"\r\nContent-Type: audio/wav\r\n\r\n");
        wav_header(wav, job->n);
        body_len = strlen(head) + sizeof(wav) + job->n * 2;
    }
    snprintf(tail, sizeof(tail), "\r\n--" BOUNDARY "--\r\n");
    body_len += strlen(tail);

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 90000,
        .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 4096, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    char auth[120];
    snprintf(auth, sizeof(auth), "Bearer %s", s_token);
    esp_http_client_set_header(c, "CF-Access-Client-Id", s_cf_id);
    esp_http_client_set_header(c, "CF-Access-Client-Secret", s_cf_secret);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "multipart/form-data; boundary=" BOUNDARY);

    int64_t t0 = esp_timer_get_time();
    if (esp_http_client_open(c, body_len) != ESP_OK) { fail = "SIN CONEXION"; goto done; }
    bool ok = write_all(c, head, strlen(head));
    if (job->texto) ok = ok && write_all(c, job->texto, strlen(job->texto));
    else ok = ok && write_all(c, wav, sizeof(wav)) && write_all(c, job->pcm, job->n * 2);
    ok = ok && write_all(c, tail, strlen(tail));
    if (!ok) { fail = "SE CORTO EL ENVIO"; goto done; }

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGI(TAG, "HTTP %d en %.1fs", status, (esp_timer_get_time() - t0) / 1e6);
        fail = http_error(status); goto done;
    }

    /* El flujo: cuadros de 1 byte de tipo + 4 de largo (big-endian) + datos
     * (canela-web, canelita.py). H oído, R respuesta, A frase, E error, Z fin. */
    bool fin = false, primera = true;
    size_t total = 0;
    while (!fin) {
        if (job->gen != s_gen) goto done;            /* turno cancelado: se suelta */
        uint8_t h[5];
        if (!read_exact(c, h, 5)) { fail = "SE CORTO LA RESPUESTA"; goto done; }
        uint32_t n = (uint32_t)h[1] << 24 | h[2] << 16 | h[3] << 8 | h[4];
        if (n > FRAME_MAX) { fail = "RESPUESTA DEMASIADO LARGA"; goto done; }
        heap_caps_free(frame);
        frame = heap_caps_malloc(n + 1, BIG);
        if (!frame) { fail = "SIN MEMORIA"; goto done; }
        if (!read_exact(c, frame, n)) { fail = "SE CORTO LA RESPUESTA"; goto done; }
        frame[n] = 0;
        switch (h[0]) {
        case 'H': {
            char heard[EV_TEXT];
            strlcpy(heard, (char *)frame, sizeof(heard));
            ascii_es(heard);
            if (heard[0] && job->gen == s_gen) emit(MUSE_HATCH_EV_HEARD, heard);
            break;
        }
        case 'R': {
            free(reply_utf8);
            reply_utf8 = strdup((char *)frame);
            char reply[TEXT_MAX];
            strlcpy(reply, (char *)frame, sizeof(reply));
            ascii_es(reply);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (job->gen == s_gen) {
                strlcpy(s_turn.text, reply, sizeof(s_turn.text));
                emit(MUSE_HATCH_EV_REPLY, reply);
            }
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "texto en %.1fs (%u caracteres)", (esp_timer_get_time() - t0) / 1e6,
                     (unsigned)strlen(reply));
            break;
        }
        case 'A': {
            if (n < 2) { fail = "AUDIO CORRUPTO"; goto done; }
            /* Dónde termina la frase en R (bytes UTF-8) → en el texto ASCII de la pantalla. */
            size_t end_utf8 = (size_t)frame[0] << 8 | frame[1], txt_end = 0;
            if (reply_utf8) {
                char tmp[TEXT_MAX];
                strlcpy(tmp, reply_utf8, end_utf8 + 1 < sizeof(tmp) ? end_utf8 + 1 : sizeof(tmp));
                ascii_es(tmp);
                txt_end = strlen(tmp);
            }
            size_t out_n = 0;
            out = mp3_to_pcm(frame + 2, n - 2, &out_n);
            if (!out || !out_n) { fail = "NO PUDE DECODIFICAR EL AUDIO"; goto done; }
            if (!append_frase(job->gen, out, out_n, txt_end)) { fail = "SIN MEMORIA"; goto done; }
            heap_caps_free(out); out = NULL;
            total += out_n;
            if (primera) {
                primera = false;
                ESP_LOGI(TAG, "primera frase en %.1fs", (esp_timer_get_time() - t0) / 1e6);
            }
            break;
        }
        case 'E':
            strlcpy(fail_buf, (char *)frame, sizeof(fail_buf));
            ascii_es(fail_buf);
            fail = fail_buf;
            goto done;
        case 'Z':
            fin = true;
            break;
        default:
            break;                                   /* tipo nuevo: se ignora */
        }
    }
    ESP_LOGI(TAG, "respuesta: %.1fs de audio, completa en %.1fs", (double)total / MUSE_AUDIO_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    if (job->gen == s_gen) emit(MUSE_HATCH_EV_DONE, "");

done:
    if (fail) {
        ESP_LOGW(TAG, "turno falló: %s", fail);
        if (job->gen == s_gen) emit(MUSE_HATCH_EV_ERROR, fail);
    }
    esp_http_client_cleanup(c);
    heap_caps_free(frame);
    free(reply_utf8);
    heap_caps_free(out);
    heap_caps_free(job->pcm);
    free(job->texto);
    free(job);
}

/* Una sola tarea permanente procesa los turnos (patrón de muse_chat_session.cpp):
 * minimp3 pone ~16 KB en la pila y TLS otro tanto, así que va con 48 KB de pila en
 * PSRAM. No se borra a sí misma: una tarea con pila en PSRAM no puede liberarse sola. */
static QueueHandle_t s_jobs;

static void worker_loop(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) procesar(job);
    }
}

static void launch(int16_t *pcm, size_t n, char *texto)
{
    job_t *job = calloc(1, sizeof(*job));
    if (!job) { heap_caps_free(pcm); free(texto); emit(MUSE_HATCH_EV_ERROR, "SIN MEMORIA"); return; }
    job->pcm = pcm; job->n = n; job->texto = texto; job->gen = s_gen;
    if (!s_jobs || xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        heap_caps_free(pcm); free(texto); free(job);
        emit(MUSE_HATCH_EV_ERROR, "OCUPADA, INTENTA DE NUEVO");
    }
}

/* ---- API pública (muse_chat.h) ---- */

void muse_hatch_start(void)
{
    s_events = xQueueCreate(8, sizeof(ev_t));
    s_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(2, sizeof(job_t *));
    if (xTaskCreatePinnedToCoreWithCaps(worker_loop, "canelita", 48 * 1024, NULL, 5, NULL, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no pude crear la tarea de canelita");
    }
    load_config();
    ESP_LOGI(TAG, "canelita lista: %s", configured() ? "configurada" : "falta configurar por USB");
}

void muse_hatch_status(muse_hatch_status_t *out)
{
    if (!configured()) {
        out->state = MUSE_HATCH_NOT_SET;
        strlcpy(out->detail, "Configura canelita por USB", sizeof(out->detail));
    } else if (!muse_wifi_connected()) {
        out->state = MUSE_HATCH_OFFLINE;
        strlcpy(out->detail, "Esperando Wi-Fi", sizeof(out->detail));
    } else {
        out->state = MUSE_HATCH_REACHABLE;
        strlcpy(out->detail, "Lista para hablar con Canela", sizeof(out->detail));
    }
}

void muse_hatch_test(void) { load_config(); }
void muse_hatch_config_changed(void) { load_config(); }
void muse_hatch_set_resting(bool resting) { (void)resting; }

const char *muse_hatch_state_name(muse_hatch_state_t state)
{
    switch (state) {
    case MUSE_HATCH_NOT_SET: return "Sin configurar";
    case MUSE_HATCH_OFFLINE: return "Sin red";
    case MUSE_HATCH_UNTESTED: return "Guardada";
    case MUSE_HATCH_TESTING: return "Conectando";
    case MUSE_HATCH_REACHABLE: return "Conectada";
    case MUSE_HATCH_UNREACHABLE: return "No conecta";
    }
    return "";
}

bool muse_hatch_ready(void)
{
    return s_events && configured() && muse_wifi_connected();
}

void muse_hatch_turn_begin(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_gen++;
    free_turn_locked();
    xQueueReset(s_events);
    s_turn.pcm = heap_caps_malloc(PCM_CAP * 2, BIG);
    s_turn.talking = s_turn.pcm != NULL;
    xSemaphoreGive(s_lock);
    if (!s_turn.pcm) emit(MUSE_HATCH_EV_ERROR, "SIN MEMORIA");
}

size_t muse_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    (void)wait_ms;
    if (!s_turn.talking) return 0;
    size_t take = PCM_CAP - s_turn.n < frames ? PCM_CAP - s_turn.n : frames;
    memcpy(s_turn.pcm + s_turn.n, pcm, take * 2);
    s_turn.n += take;
    return frames;                       /* lo que pase del tope se descarta */
}

void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    muse_hatch_turn_audio_wait(pcm, frames, 0);
}

void muse_hatch_turn_end(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool talking = s_turn.talking;
    int16_t *pcm = s_turn.pcm;
    size_t n = s_turn.n;
    s_turn.pcm = NULL; s_turn.n = 0; s_turn.talking = false;
    xSemaphoreGive(s_lock);
    if (!talking || !pcm) {
        heap_caps_free(pcm);
        emit(MUSE_HATCH_EV_ERROR, "NO SE GRABO NADA");
        return;
    }
    ESP_LOGI(TAG, "mandando %.1fs de voz", (double)n / MIC_RATE);
    launch(pcm, n, NULL);
}

void muse_hatch_turn_cancel(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_gen++;                             /* la tarea en curso descarta su resultado */
    free_turn_locked();
    xSemaphoreGive(s_lock);
    if (s_events) xQueueReset(s_events);
}

muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    ev_t ev;
    if (!s_events || xQueueReceive(s_events, &ev, 0) != pdTRUE) return MUSE_HATCH_EV_NONE;
    strlcpy(text, ev.text, cap);
    return ev.type;
}

bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    /* La frase que suena, y dentro de ella en proporción: el audio total aún
     * no se sabe mientras llegan las frases. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t len = strlen(s_turn.text);
    bool ok = false;
    if (len && s_turn.nseg) {
        int k = 0;
        while (k < s_turn.nseg - 1 && played >= s_turn.seg_pcm[k]) k++;
        size_t p0 = k ? s_turn.seg_pcm[k - 1] : 0, t0 = k ? s_turn.seg_txt[k - 1] : 0;
        size_t p1 = s_turn.seg_pcm[k], t1 = s_turn.seg_txt[k];
        size_t at = t1;
        if (played < p1 && p1 > p0) at = t0 + (size_t)((double)(played - p0) * (t1 - t0) / (p1 - p0));
        ok = muse_hatch_caption_at(s_turn.text, at < len ? at : len - 1, out, cap);
    }
    xSemaphoreGive(s_lock);
    return ok;
}

size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t left = s_turn.out ? s_turn.out_n - s_turn.out_rd : 0;
    size_t n = left < frames ? left : frames;
    if (n) {
        memcpy(pcm, s_turn.out + s_turn.out_rd, n * 2);
        s_turn.out_rd += n;
    }
    xSemaphoreGive(s_lock);
    if (!n && wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms) ? pdMS_TO_TICKS(wait_ms) : 1);
    return n;
}

size_t muse_hatch_mp3_selftest(int16_t **pcm)
{
    *pcm = NULL;
    return 0;
}

/* Turno por texto (consola: chat=…). Libera text, como espera muse_input.c. */
void muse_hatch_text_turn(char *text)
{
    if (!muse_hatch_ready()) {
        free(text);
        emit(MUSE_HATCH_EV_ERROR, configured() ? "SIN RED" : "SIN CONFIGURAR");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_gen++;
    free_turn_locked();
    xQueueReset(s_events);
    xSemaphoreGive(s_lock);
    launch(NULL, 0, text);
}

void muse_hatch_text_cancel(void) { muse_hatch_turn_cancel(); }

/* ---- Consola: configuración por USB (nunca imprime valores) ---- */

static bool set_key(const char *k, const char *v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, k, v) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool canela_console(char *line)
{
    if (strncmp(line, "canelita.", 9)) return false;
    char *cmd = line + 9;
    if (!strncmp(cmd, "set ", 4)) {
        char *k = cmd + 4, *v = strchr(k, ' ');
        if (!v) { printf("@canelita.error uso: canelita.set <clave> <valor>\n"); return true; }
        *v++ = 0;
        static const char *keys[] = { "url", "cf_id", "cf_secret", "token" };
        bool known = false;
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) known |= !strcmp(k, keys[i]);
        if (!known) { printf("@canelita.error clave desconocida: %s\n", k); return true; }
        bool ok = set_key(k, v);
        load_config();
        printf(ok ? "@canelita.ok %s (%u caracteres)\n" : "@canelita.error no pude guardar %s\n",
               k, (unsigned)strlen(v));
        return true;
    }
    if (!strncmp(cmd, "wifi ", 5)) {
        /* canelita.wifi <ssid>\t<contraseña>: el tabulador permite espacios en el SSID. */
        char *ssid = cmd + 5, *pass = strchr(ssid, '\t');
        if (!pass) { printf("@canelita.error uso: canelita.wifi <ssid>\\t<contraseña>\n"); return true; }
        *pass++ = 0;
        bool ok = muse_link_wifi_set(ssid, pass);
        muse_wifi_apply();
        printf(ok ? "@canelita.ok wifi %s\n" : "@canelita.error no pude guardar la red %s\n", ssid);
        return true;
    }
    if (!strcmp(cmd, "show")) {
        muse_wifi_status_t w;
        muse_wifi_status(&w);
        printf("@canelita {\"url\":%s,\"cf_id\":%s,\"cf_secret\":%s,\"token\":%s,\"wifi\":%d,\"ssid\":\"%s\",\"ip\":\"%s\",\"detalle\":\"%s\",\"lista\":%s}\n",
               s_url[0] ? "true" : "false", s_cf_id[0] ? "true" : "false",
               s_cf_secret[0] ? "true" : "false", s_token[0] ? "true" : "false",
               (int)w.state, w.ssid, w.ip, w.detail, muse_hatch_ready() ? "true" : "false");
        return true;
    }
    if (!strcmp(cmd, "scan")) {
        /* Diagnóstico: qué redes ve la placa (sólo 2.4 GHz; el S3 no tiene 5 GHz). */
        static muse_wifi_ap_t aps[24];
        uint32_t gen = 0;
        if (muse_wifi_scan() != ESP_OK) { printf("@canelita.error no pude escanear\n"); return true; }
        for (int i = 0; i < 100 && muse_wifi_scanning(); i++) vTaskDelay(pdMS_TO_TICKS(100));
        int n = muse_wifi_scan_results(aps, sizeof(aps) / sizeof(*aps), &gen);
        printf("@canelita.scan [");
        for (int i = 0; i < n; i++) {
            printf("%s{\"ssid\":\"%s\",\"rssi\":%d,\"segura\":%s}", i ? "," : "", aps[i].ssid, aps[i].rssi,
                   aps[i].secure ? "true" : "false");
        }
        printf("]\n");
        return true;
    }
    if (!strcmp(cmd, "trazas")) {
        /* Diagnóstico de congelamientos: la pila de TODAS las tareas, en vivo. La
         * consola sigue viva aunque la interfaz o la voz se traben. Decodificar con
         * xtensa-esp32s3-elf-addr2line -pfiaC -e build-175c/muse-gadget.elf <dirs>. */
        printf("@canelita.trazas inicio\n");
        fflush(stdout);
        esp_backtrace_print_all_tasks(24);
        printf("@canelita.trazas fin\n");
        return true;
    }
    if (!strncmp(cmd, "vol ", 4)) {
        int v = atoi(cmd + 4);
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        muse_settings_set_volume(v);
        printf("@canelita.ok vol %d\n", v);
        return true;
    }
    printf("@canelita.error comando desconocido\n");
    return true;
}
