/*
 * Copyright (c) 2026 Juan Manuel Fraga.
 * SPDX-License-Identifier: Apache-2.0
 *
 * canelita: la mascota de bolsillo, una página del colgante (a la izquierda del reloj).
 *
 * Canelita es un tamagotchi que vive del hábito de ejercicio de quien la lleva: el estado lo
 * calcula el brain (GET /v1/agentes/canela/mascota, renglones `clave<TAB>valor`) y aquí
 * solo se pinta. Comer y jugar se tocan en pantalla; el ejercicio se le cuenta a Canela
 * por voz, o se anota aquí manteniendo «+15 MIN» (mantener, para que un roce no cuente).
 * El dibujo sale de tools/canelita/mascota/sprites.py.
 */
#include "canela_mascota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "canela_chat.h"
#include "canela_mascota_sprites.h"

static const char *TAG = "mascota";

#define COLOR_ACCENT 0xF2A65A
#define COLOR_DIM 0x8A8A8A
#define ESCALA_MAX 8          /* 24x22 → 192x176 px en la adulta */
#define AVISO_S 3.0f          /* «ÑAM» y compañía, antes de volver a la frase del brain */

typedef struct {
    char cara[16], frase[64], etapa[16], nombre[16];
    int animo, hambre, aburrimiento, min_semana, meta, dias_activos, racha;
} estado_t;

static lv_obj_t *s_tile, *s_img, *s_nombre, *s_etapa, *s_frase, *s_semana, *s_barra, *s_zzz;
static lv_obj_t *s_btn_comer, *s_btn_jugar, *s_btn_ejercicio;
static lv_image_dsc_t s_dsc;
static uint16_t *s_px;
static estado_t s_e;
static int s_cara = -1, s_cuadro = -1, s_escala_pintada = -1;
static float s_aviso_hasta;
static bool s_hay_estado;

static lv_obj_t *etiqueta(lv_obj_t *padre, const lv_font_t *f, uint32_t color)
{
    lv_obj_t *l = lv_label_create(padre);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

static int cara_indice(const char *cara)
{
    for (int i = 0; i < MASCOTA_N_CARAS; i++) {
        if (!strcmp(cara, MASCOTA_CARAS[i])) return i;
    }
    return 2;   /* tranquila */
}

static int escala_de(const char *etapa)
{
    if (!strcmp(etapa, "CACHORRA")) return 6;
    if (!strcmp(etapa, "JOVEN")) return 7;
    return ESCALA_MAX;
}

/* Pinta la cara y el cuadro de la cola en el búfer de la imagen, centrada en el área de la adulta. */
static void pintar(int cara, int cuadro, int escala)
{
    if (!s_px) return;
    int ancho = MASCOTA_W * ESCALA_MAX, alto = MASCOTA_H * ESCALA_MAX;
    memset(s_px, 0, (size_t)ancho * alto * 2);
    int ox = (ancho - MASCOTA_W * escala) / 2, oy = alto - MASCOTA_H * escala;   /* de pie en el piso */
    const uint8_t *px = MASCOTA_PX[cara][cuadro];
    for (int y = 0; y < MASCOTA_H; y++) {
        for (int x = 0; x < MASCOTA_W; x++) {
            uint8_t i = px[y * MASCOTA_W + x];
            if (!i) continue;
            uint32_t c = MASCOTA_PALETA[i];
            uint16_t v = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            for (int dy = 0; dy < escala; dy++) {
                uint16_t *fila = s_px + (size_t)(oy + y * escala + dy) * ancho + ox + x * escala;
                for (int dx = 0; dx < escala; dx++) fila[dx] = v;
            }
        }
    }
    lv_obj_invalidate(s_img);
}

static void aviso(const char *texto)
{
    lv_label_set_text(s_frase, texto);
    s_aviso_hasta = (float)esp_timer_get_time() / 1e6f + AVISO_S;
}

static void on_comer(lv_event_t *e)
{
    (void)e;
    aviso(canela_mascota_accion("comer", 0) ? "NAM NAM!" : "UN MOMENTO...");
}

static void on_jugar(lv_event_t *e)
{
    (void)e;
    aviso(canela_mascota_accion("jugar", 0) ? "GUAU! A JUGAR!" : "UN MOMENTO...");
}

static void on_ejercicio(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED) {
        aviso("MANTEN PARA ANOTAR 15 MIN");
        return;
    }
    aviso(canela_mascota_accion("ejercicio", 15) ? "+15 MIN! BIEN!" : "UN MOMENTO...");
}

static lv_obj_t *boton(lv_obj_t *padre, const char *texto, lv_event_cb_t cb, lv_event_code_t code)
{
    lv_obj_t *b = lv_button_create(padre);
    lv_obj_set_size(b, 104, 46);
    lv_obj_set_style_radius(b, 23, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A1A10), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x5A3418), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, code, NULL);
    lv_obj_t *l = etiqueta(b, &lv_font_unscii_8, 0xFFFFFF);
    lv_label_set_text(l, texto);
    lv_obj_center(l);
    return b;
}

void canela_mascota_build(lv_obj_t *tile, int w, int h)
{
    s_tile = tile;
    int cy = h / 2;
    s_nombre = etiqueta(tile, &lv_font_unscii_16, COLOR_ACCENT);
    lv_obj_set_style_text_letter_space(s_nombre, 2, 0);
    lv_label_set_text(s_nombre, "CANELITA");
    lv_obj_align(s_nombre, LV_ALIGN_TOP_MID, 0, 34);
    s_etapa = etiqueta(tile, &lv_font_unscii_8, COLOR_DIM);
    lv_obj_align(s_etapa, LV_ALIGN_TOP_MID, 0, 56);

    int ancho = MASCOTA_W * ESCALA_MAX, alto = MASCOTA_H * ESCALA_MAX;
    s_px = heap_caps_calloc((size_t)ancho * alto, 2, MALLOC_CAP_SPIRAM);
    if (!s_px) ESP_LOGE(TAG, "sin memoria para la mascota");
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = ancho;
    s_dsc.header.h = alto;
    s_dsc.header.stride = ancho * 2;
    s_dsc.data = (const uint8_t *)s_px;
    s_dsc.data_size = (uint32_t)ancho * alto * 2;
    s_img = lv_image_create(tile);
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_img, on_jugar, LV_EVENT_CLICKED, NULL);   /* tocarla también es jugar */

    s_zzz = etiqueta(tile, &lv_font_unscii_16, 0x9FB4FF);
    lv_label_set_text(s_zzz, "Z z");
    lv_obj_align(s_zzz, LV_ALIGN_TOP_MID, 70, 84);
    lv_obj_add_flag(s_zzz, LV_OBJ_FLAG_HIDDEN);

    s_frase = etiqueta(tile, &lv_font_unscii_16, 0xFFFFFF);
    lv_obj_set_width(s_frase, w - 120);
    lv_label_set_long_mode(s_frase, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_frase, "CARGANDO...");
    lv_obj_align(s_frase, LV_ALIGN_TOP_MID, 0, 72 + alto + 10);

    s_semana = etiqueta(tile, &lv_font_unscii_8, COLOR_DIM);
    lv_obj_align(s_semana, LV_ALIGN_CENTER, 0, 108);
    s_barra = lv_bar_create(tile);
    lv_obj_set_size(s_barra, 240, 10);
    lv_obj_align(s_barra, LV_ALIGN_CENTER, 0, 124);
    lv_bar_set_range(s_barra, 0, 100);
    lv_obj_set_style_bg_color(s_barra, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_barra, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);

    s_btn_comer = boton(tile, "COMER", on_comer, LV_EVENT_CLICKED);
    lv_obj_align(s_btn_comer, LV_ALIGN_CENTER, -112, 162);
    s_btn_jugar = boton(tile, "JUGAR", on_jugar, LV_EVENT_CLICKED);
    lv_obj_align(s_btn_jugar, LV_ALIGN_CENTER, 0, 162);
    s_btn_ejercicio = boton(tile, "+15 MIN", on_ejercicio, LV_EVENT_LONG_PRESSED);
    lv_obj_add_event_cb(s_btn_ejercicio, on_ejercicio, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_align(s_btn_ejercicio, LV_ALIGN_CENTER, 112, 162);
    (void)cy;
}

static void leer_estado(char *crudo)
{
    estado_t e = s_e;
    for (char *l = crudo, *sig; l && *l; l = sig) {
        sig = strchr(l, '\n');
        if (sig) *sig++ = 0;
        char *tab = strchr(l, '\t');
        if (!tab) continue;
        *tab++ = 0;
        if (!strcmp(l, "cara")) strlcpy(e.cara, tab, sizeof(e.cara));
        else if (!strcmp(l, "frase")) strlcpy(e.frase, tab, sizeof(e.frase));
        else if (!strcmp(l, "etapa")) strlcpy(e.etapa, tab, sizeof(e.etapa));
        else if (!strcmp(l, "nombre")) strlcpy(e.nombre, tab, sizeof(e.nombre));
        else if (!strcmp(l, "animo")) e.animo = atoi(tab);
        else if (!strcmp(l, "hambre")) e.hambre = atoi(tab);
        else if (!strcmp(l, "aburrimiento")) e.aburrimiento = atoi(tab);
        else if (!strcmp(l, "min_semana")) e.min_semana = atoi(tab);
        else if (!strcmp(l, "meta_semanal_min")) e.meta = atoi(tab);
        else if (!strcmp(l, "dias_activos_semana")) e.dias_activos = atoi(tab);
        else if (!strcmp(l, "racha_semanas")) e.racha = atoi(tab);
    }
    s_e = e;
    s_hay_estado = e.cara[0] != 0;
}

static void boton_alerta(lv_obj_t *b, bool alerta)
{
    lv_obj_set_style_border_color(b, lv_color_hex(alerta ? 0xFF5A5A : COLOR_ACCENT), 0);
}

void canela_mascota_tick(float now, bool visible)
{
    if (!s_tile || !visible) return;
    canela_mascota_pedir(60);
    static char crudo[1024];
    int edad = 0;
    bool fallo = false, pend = false;
    if (canela_mascota_leer(crudo, sizeof(crudo), &edad, &fallo, &pend)) {
        leer_estado(crudo);
    } else if (fallo && !s_hay_estado) {
        lv_label_set_text(s_frase, "SIN CONEXION");
    }
    if (!s_hay_estado) {
        if (s_cara < 0) {
            s_cara = 2;
            pintar(2, 0, 6);
        }
        return;
    }

    int cara = cara_indice(s_e.cara);
    bool dormida = !strcmp(s_e.cara, "dormida");
    /* La cola se mueve más rápido mientras más contenta está; dormida no se mueve. */
    float periodo = dormida ? 1e9f : s_e.animo >= 70 ? 0.25f : s_e.animo >= 40 ? 0.5f : 1.0f;
    int cuadro = dormida ? 0 : (int)(now / periodo) % 2;
    int escala = escala_de(s_e.etapa);
    if (cara != s_cara || cuadro != s_cuadro || escala != s_escala_pintada) {
        pintar(cara, cuadro, escala);
        s_cara = cara;
        s_cuadro = cuadro;
        s_escala_pintada = escala;
    }
    lv_obj_set_flag(s_zzz, LV_OBJ_FLAG_HIDDEN, !dormida);

    char buf[64];
    if (s_e.racha > 0) snprintf(buf, sizeof(buf), "%s - RACHA %d SEM", s_e.etapa, s_e.racha);
    else strlcpy(buf, s_e.etapa, sizeof(buf));
    if (strcmp(buf, lv_label_get_text(s_etapa))) lv_label_set_text(s_etapa, buf);
    if (s_e.nombre[0] && strcmp(s_e.nombre, lv_label_get_text(s_nombre))) lv_label_set_text(s_nombre, s_e.nombre);
    if (now >= s_aviso_hasta && strcmp(s_e.frase, lv_label_get_text(s_frase))) lv_label_set_text(s_frase, s_e.frase);
    snprintf(buf, sizeof(buf), "SEMANA %d / %d MIN  -  %d DIAS", s_e.min_semana, s_e.meta, s_e.dias_activos);
    if (strcmp(buf, lv_label_get_text(s_semana))) lv_label_set_text(s_semana, buf);
    int pct = s_e.meta > 0 ? s_e.min_semana * 100 / s_e.meta : 0;
    if (pct > 100) pct = 100;
    if (lv_bar_get_value(s_barra) != pct) lv_bar_set_value(s_barra, pct, LV_ANIM_OFF);
    boton_alerta(s_btn_comer, s_e.hambre >= 80);
    boton_alerta(s_btn_jugar, s_e.aburrimiento >= 80);
    (void)edad;
    (void)pend;
}
