/*
 * Copyright (c) 2026 Juan Manuel Fraga.
 * SPDX-License-Identifier: Apache-2.0
 *
 * canelita: la mascota de bolsillo, una página del colgante (a la izquierda del reloj).
 *
 * Canelita vive del día de quien la lleva: gana huesitos con lo que de verdad hace (moverse,
 * comer bien, agua, pausas, dormir). Se anota en el menú ANOTAR de aquí o contándoselo a
 * Canela; la CORREA mide un paseo de verdad. Con los huesitos se compran accesorios
 * (mantener a la perrita abre la tienda); tocarla abre ANOTAR. Nada de tamagotchi: solo los hábitos
 * la transforman. El estado y lo que dice los
 * calcula el brain (GET/POST /v1/agentes/canela/mascota, renglones `clave<TAB>valor`); aquí
 * solo se pinta. El dibujo sale de tools/canelita/mascota/sprites.py.
 */
#include "canela_mascota.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "canela_chat.h"
#include "canela_mascota_sprites.h"

static const char *TAG = "mascota";

#define COLOR_ACCENT 0xF2A65A
#define COLOR_DIM 0x8A8A8A
#define COLOR_HUESO 0xF2C230
#define ESCALA_MAX 7          /* 24x22 → 168x154 px en la adulta */
#define AVISO_S 4.0f          /* lo que respondió una acción, antes de volver a la frase del brain */
#define MENU_MAX 7

typedef struct {
    char cara[16], frase[72], etapa[16], nombre[16], contexto[8], puesto[12], forma[16], tendencia[16];
    char accesorios[64];
    int animo, condicion, nutricion, energia, huesitos, huesitos_hoy, paseando;
    int precio[MASCOTA_N_ACC];
} estado_t;

static lv_obj_t *s_tile, *s_img, *s_nombre, *s_etapa, *s_huesos, *s_frase, *s_zzz;
static lv_obj_t *s_barra[3];
static lv_obj_t *s_btn_correa, *s_lbl_correa;
static lv_obj_t *s_capa, *s_capa_titulo, *s_op[MENU_MAX], *s_op_lbl[MENU_MAX];
static lv_image_dsc_t s_dsc;
static uint16_t *s_px;
EXT_RAM_BSS_ATTR static estado_t s_e;
static int s_cara = -1, s_cuadro = -1, s_escala_pintada = -1, s_acc_pintado = -2;
static float s_aviso_hasta;
static bool s_hay_estado;

/* ---------- menús (ANOTAR y la TIENDA comparten una capa) ---------- */

typedef enum { M_NADA, M_ANOTAR, M_MOVER, M_MINUTOS, M_COMER, M_DORMI, M_TIENDA } menu_t;
static menu_t s_menu = M_NADA;
static char s_tipo[12];   /* el ejercicio elegido, mientras se escogen los minutos */

static const char *const ANOTAR[] = { "MOVERME", "COMER", "AGUA", "PAUSA", "DORMI" };
static const char *const MOVER[] = { "CAMINAR", "SENDERISMO", "BICI", "REMADORA", "CALISTENIA" };   /* los suyos */
static const int MINUTOS[] = { 15, 30, 45, 60, 90 };
static const char *const COMER[] = { "BIEN", "REGULAR", "ANTOJO" };
static const char *const COMER_ACC[] = { "comida:bien", "comida:regular", "comida:antojo" };
static const int HORAS[] = { 5, 6, 7, 8, 9 };

static lv_obj_t *etiqueta(lv_obj_t *padre, const lv_font_t *f, uint32_t color)
{
    lv_obj_t *l = lv_label_create(padre);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

static void set_texto(lv_obj_t *l, const char *t)
{
    if (strcmp(t, lv_label_get_text(l))) lv_label_set_text(l, t);
}

static int cara_indice(const char *cara)
{
    for (int i = 0; i < MASCOTA_N_CARAS; i++) {
        if (!strcmp(cara, MASCOTA_CARAS[i])) return i;
    }
    return 2;   /* tranquila */
}

static int acc_indice(const char *acc)
{
    for (int i = 0; i < MASCOTA_N_ACC; i++) {
        if (!strcmp(acc, MASCOTA_ACC[i])) return i;
    }
    return -1;
}

static int escala_de(const char *etapa)
{
    if (!strcmp(etapa, "CACHORRA")) return 5;
    if (!strcmp(etapa, "JOVEN")) return 6;
    return ESCALA_MAX;
}

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}

static void bloque(int ox, int oy, int escala, int x, int y, uint16_t v)
{
    int ancho = MASCOTA_W * ESCALA_MAX;
    for (int dy = 0; dy < escala; dy++) {
        uint16_t *fila = s_px + (size_t)(oy + y * escala + dy) * ancho + ox + x * escala;
        for (int dx = 0; dx < escala; dx++) fila[dx] = v;
    }
}

/* La cara, el cuadro de la cola y el accesorio puesto, de pie en el piso del área de la adulta. */
static void pintar(int cara, int cuadro, int escala, int acc)
{
    if (!s_px) return;
    int ancho = MASCOTA_W * ESCALA_MAX, alto = MASCOTA_H * ESCALA_MAX;
    memset(s_px, 0, (size_t)ancho * alto * 2);
    int ox = (ancho - MASCOTA_W * escala) / 2, oy = alto - MASCOTA_H * escala;
    const uint8_t *px = MASCOTA_PX[cara][cuadro];
    for (int y = 0; y < MASCOTA_H; y++) {
        for (int x = 0; x < MASCOTA_W; x++) {
            uint8_t i = px[y * MASCOTA_W + x];
            if (i) bloque(ox, oy, escala, x, y, rgb565(MASCOTA_PALETA[i]));
        }
    }
    if (acc >= 0) {
        for (const uint8_t *p = MASCOTA_ACC_PX[MASCOTA_ACC_INICIO[acc]]; p[0] != 255; p += 3) {
            bloque(ox, oy, escala, p[1], p[0], rgb565(MASCOTA_PALETA[p[2]]));
        }
    }
    lv_obj_invalidate(s_img);
}

static void aviso(const char *texto, float segundos)
{
    lv_label_set_text(s_frase, texto);
    s_aviso_hasta = (float)esp_timer_get_time() / 1e6f + segundos;
}

/* Manda una acción; la respuesta del brain trae su aviso («+10 HUESITOS. NAM!») como frase. */
static void pedir(const char *accion)
{
    if (canela_mascota_accion(accion, 0)) aviso("ANOTANDO...", 8.0f);
    else aviso("UN MOMENTO...", AVISO_S);
}

static void mostrar_menu(menu_t m);

static void on_op(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    static char accion[32];
    switch (s_menu) {
    case M_ANOTAR:
        if (i == 0) { mostrar_menu(M_MOVER); return; }
        if (i == 1) { mostrar_menu(M_COMER); return; }
        if (i == 4) { mostrar_menu(M_DORMI); return; }
        mostrar_menu(M_NADA);
        pedir(i == 2 ? "agua" : "pausa");
        return;
    case M_MOVER:
        strlcpy(s_tipo, MOVER[i], sizeof(s_tipo));
        for (char *c = s_tipo; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        mostrar_menu(M_MINUTOS);
        return;
    case M_MINUTOS:
        snprintf(accion, sizeof(accion), "ej:%s:%d", s_tipo, MINUTOS[i]);
        break;
    case M_COMER:
        strlcpy(accion, COMER_ACC[i], sizeof(accion));
        break;
    case M_DORMI:
        snprintf(accion, sizeof(accion), "sueno:%d", HORAS[i]);
        break;
    case M_TIENDA:
        if (i < MASCOTA_N_ACC) snprintf(accion, sizeof(accion), "comprar:%s", MASCOTA_ACC[i]);
        else strlcpy(accion, "poner:nada", sizeof(accion));
        break;
    default:
        return;
    }
    mostrar_menu(M_NADA);
    pedir(accion);
}

static void on_volver(lv_event_t *e)
{
    (void)e;
    mostrar_menu(s_menu == M_MINUTOS ? M_MOVER : (s_menu == M_MOVER || s_menu == M_COMER || s_menu == M_DORMI)
                                                     ? M_ANOTAR : M_NADA);
}

static void pintar_tienda(void);

/* Llena la capa con las opciones del menú `m` (o la esconde con M_NADA). */
static void mostrar_menu(menu_t m)
{
    s_menu = m;
    lv_obj_set_flag(s_capa, LV_OBJ_FLAG_HIDDEN, m == M_NADA);
    if (m == M_NADA) return;
    const char *titulo = "";
    int n = 0;
    char buf[MENU_MAX][28];
    switch (m) {
    case M_ANOTAR:
        titulo = "QUE HICISTE?";
        for (n = 0; n < 5; n++) strlcpy(buf[n], ANOTAR[n], sizeof(buf[n]));
        break;
    case M_MOVER:
        titulo = "QUE EJERCICIO?";
        for (n = 0; n < (int)(sizeof(MOVER) / sizeof(*MOVER)); n++) strlcpy(buf[n], MOVER[n], sizeof(buf[n]));
        break;
    case M_MINUTOS:
        titulo = "CUANTO TIEMPO?";
        for (n = 0; n < 5; n++) snprintf(buf[n], sizeof(buf[n]), "%d MIN", MINUTOS[n]);
        break;
    case M_COMER:
        titulo = "COMO COMISTE?";
        for (n = 0; n < 3; n++) strlcpy(buf[n], COMER[n], sizeof(buf[n]));
        break;
    case M_DORMI:
        titulo = "CUANTO DORMISTE?";
        for (n = 0; n < 5; n++) snprintf(buf[n], sizeof(buf[n]), "%d HORAS", HORAS[n]);
        break;
    case M_TIENDA:
        titulo = "TIENDA";
        n = MASCOTA_N_ACC + 1;
        break;
    default:
        break;
    }
    lv_label_set_text(s_capa_titulo, titulo);
    /* Dos columnas, centradas en el círculo. */
    int filas = (n + 1) / 2;
    int y0 = -((filas - 1) * 56) / 2 - 10;
    for (int i = 0; i < MENU_MAX; i++) {
        bool usa = i < n;
        lv_obj_set_flag(s_op[i], LV_OBJ_FLAG_HIDDEN, !usa);
        if (!usa) continue;
        if (m != M_TIENDA) lv_label_set_text(s_op_lbl[i], buf[i]);
        bool sola = (n % 2) && i == n - 1;   /* la última, sola en su fila, al centro */
        int x = sola ? 0 : (i % 2 ? 76 : -76);
        lv_obj_set_width(s_op[i], m == M_TIENDA ? 300 : 140);
        lv_obj_align(s_op[i], LV_ALIGN_CENTER, m == M_TIENDA ? 0 : x,
                     m == M_TIENDA ? -110 + i * 50 : y0 + (i / 2) * 56);
    }
    if (m == M_TIENDA) pintar_tienda();
}

void canela_mascota_banco(const char *menu)
{
    mostrar_menu(!strcmp(menu, "anotar") ? M_ANOTAR : !strcmp(menu, "mover") ? M_MOVER
                 : !strcmp(menu, "minutos") ? M_MINUTOS : !strcmp(menu, "tienda") ? M_TIENDA : M_NADA);
}

static void on_anotar(lv_event_t *e)
{
    (void)e;
    mostrar_menu(M_ANOTAR);
}

static void on_correa(lv_event_t *e)
{
    (void)e;
    pedir("correa");
}

/* Solo los hábitos la transforman: tocarla es contarle qué hiciste. */
static void on_tocar(lv_event_t *e)
{
    (void)e;
    if (s_menu == M_NADA) mostrar_menu(M_ANOTAR);
}

static void on_mantener(lv_event_t *e)
{
    (void)e;
    mostrar_menu(M_TIENDA);
}

static lv_obj_t *boton(lv_obj_t *padre, const char *texto, int w, lv_event_cb_t cb, void *dato, lv_obj_t **lbl)
{
    lv_obj_t *b = lv_button_create(padre);
    lv_obj_set_size(b, w, 44);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A1A10), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x5A3418), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, dato);
    lv_obj_t *l = etiqueta(b, &lv_font_unscii_8, 0xFFFFFF);
    lv_label_set_text(l, texto);
    lv_obj_center(l);
    if (lbl) *lbl = l;
    return b;
}

static void build_capa(lv_obj_t *tile, int w, int h)
{
    s_capa = lv_obj_create(tile);
    lv_obj_remove_style_all(s_capa);
    lv_obj_set_size(s_capa, w, h);
    lv_obj_set_style_bg_color(s_capa, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_capa, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_capa, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_capa, LV_OBJ_FLAG_SCROLLABLE);
    s_capa_titulo = etiqueta(s_capa, &lv_font_unscii_16, COLOR_HUESO);
    lv_obj_align(s_capa_titulo, LV_ALIGN_TOP_MID, 0, 62);
    for (int i = 0; i < MENU_MAX; i++) {
        s_op[i] = boton(s_capa, "", 140, on_op, (void *)(intptr_t)i, &s_op_lbl[i]);
        lv_obj_add_flag(s_op[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *volver = boton(s_capa, "VOLVER", 120, on_volver, NULL, NULL);
    lv_obj_align(volver, LV_ALIGN_BOTTOM_MID, 0, -58);
}

void canela_mascota_build(lv_obj_t *tile, int w, int h)
{
    s_tile = tile;
    s_nombre = etiqueta(tile, &lv_font_unscii_16, COLOR_ACCENT);
    lv_obj_set_style_text_letter_space(s_nombre, 2, 0);
    lv_label_set_text(s_nombre, "CANELITA");
    lv_obj_align(s_nombre, LV_ALIGN_TOP_MID, 0, 30);
    s_etapa = etiqueta(tile, &lv_font_unscii_8, COLOR_DIM);
    lv_obj_align(s_etapa, LV_ALIGN_TOP_MID, 0, 50);
    s_huesos = etiqueta(tile, &lv_font_unscii_16, COLOR_HUESO);
    lv_obj_align(s_huesos, LV_ALIGN_TOP_MID, 0, 64);

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
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_img, on_tocar, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_img, on_mantener, LV_EVENT_LONG_PRESSED, NULL);

    s_zzz = etiqueta(tile, &lv_font_unscii_16, 0x9FB4FF);
    lv_label_set_text(s_zzz, "Z z");
    lv_obj_align(s_zzz, LV_ALIGN_TOP_MID, 80, 96);
    lv_obj_add_flag(s_zzz, LV_OBJ_FLAG_HIDDEN);

    s_frase = etiqueta(tile, &lv_font_unscii_16, 0xFFFFFF);
    lv_obj_set_width(s_frase, w - 110);
    lv_obj_set_style_text_line_space(s_frase, 2, 0);
    lv_label_set_long_mode(s_frase, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_frase, "CARGANDO...");
    lv_obj_align(s_frase, LV_ALIGN_TOP_MID, 0, 86 + alto + 6);

    /* Tres barras: moverse, comer, descansar. */
    static const char *const NOMBRES[3] = { "MOVER", "COMER", "DESCANSO" };
    static const uint32_t COLORES[3] = { 0x6FCF7A, 0xF2A65A, 0x7FB4FF };
    for (int i = 0; i < 3; i++) {
        int x = (i - 1) * 100;
        s_barra[i] = lv_bar_create(tile);
        lv_obj_set_size(s_barra[i], 84, 8);
        lv_obj_align(s_barra[i], LV_ALIGN_CENTER, x, 88);
        lv_bar_set_range(s_barra[i], 0, 100);
        lv_obj_set_style_bg_color(s_barra[i], lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_barra[i], lv_color_hex(COLORES[i]), LV_PART_INDICATOR);
        lv_obj_t *l = etiqueta(tile, &lv_font_unscii_8, COLOR_DIM);
        lv_label_set_text(l, NOMBRES[i]);
        lv_obj_align(l, LV_ALIGN_CENTER, x, 102);
    }

    s_btn_correa = boton(tile, "CORREA", 140, on_correa, NULL, &s_lbl_correa);
    lv_obj_align(s_btn_correa, LV_ALIGN_CENTER, -76, 142);
    lv_obj_t *anotar = boton(tile, "ANOTAR", 140, on_anotar, NULL, NULL);
    lv_obj_align(anotar, LV_ALIGN_CENTER, 76, 142);
    lv_obj_t *pista = etiqueta(tile, &lv_font_unscii_8, 0x55506E);
    lv_label_set_text(pista, "TOCALA: ANOTAR  -  MANTEN: TIENDA");
    lv_obj_align(pista, LV_ALIGN_CENTER, 0, 180);

    build_capa(tile, w, h);
}

static void leer_estado(char *crudo)
{
    estado_t e = s_e;
    e.paseando = -1;
    e.puesto[0] = 0;
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
        else if (!strcmp(l, "contexto")) strlcpy(e.contexto, tab, sizeof(e.contexto));
        else if (!strcmp(l, "puesto")) strlcpy(e.puesto, tab, sizeof(e.puesto));
        else if (!strcmp(l, "forma")) strlcpy(e.forma, tab, sizeof(e.forma));
        else if (!strcmp(l, "tendencia")) strlcpy(e.tendencia, tab, sizeof(e.tendencia));
        else if (!strcmp(l, "accesorios")) strlcpy(e.accesorios, tab, sizeof(e.accesorios));
        else if (!strcmp(l, "animo")) e.animo = atoi(tab);
        else if (!strcmp(l, "condicion")) e.condicion = atoi(tab);
        else if (!strcmp(l, "nutricion")) e.nutricion = atoi(tab);
        else if (!strcmp(l, "energia")) e.energia = atoi(tab);
        else if (!strcmp(l, "huesitos")) e.huesitos = atoi(tab);
        else if (!strcmp(l, "huesitos_hoy")) e.huesitos_hoy = atoi(tab);
        else if (!strcmp(l, "paseando")) e.paseando = atoi(tab);
        else if (!strcmp(l, "tienda")) {
            char *guardar = NULL;
            for (char *it = strtok_r(tab, ",", &guardar); it; it = strtok_r(NULL, ",", &guardar)) {
                char *dp = strchr(it, ':');
                if (!dp) continue;
                *dp = 0;
                int i = acc_indice(it);
                if (i >= 0) e.precio[i] = atoi(dp + 1);
            }
        }
    }
    s_e = e;
    s_hay_estado = e.cara[0] != 0;
}

static bool tiene(const char *acc)
{
    char lista[sizeof(s_e.accesorios) + 2], buscado[20];
    snprintf(lista, sizeof(lista), ",%s,", s_e.accesorios);
    snprintf(buscado, sizeof(buscado), ",%s,", acc);
    return strstr(lista, buscado) != NULL;
}

static void pintar_tienda(void)
{
    char buf[48];
    for (int i = 0; i < MASCOTA_N_ACC; i++) {
        char nom[16];
        strlcpy(nom, MASCOTA_ACC[i], sizeof(nom));
        for (char *c = nom; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        if (!strcmp(s_e.puesto, MASCOTA_ACC[i])) snprintf(buf, sizeof(buf), "%s - PUESTO", nom);
        else if (tiene(MASCOTA_ACC[i])) snprintf(buf, sizeof(buf), "%s - PONER", nom);
        else snprintf(buf, sizeof(buf), "%s - %d HUESITOS", nom, s_e.precio[i]);
        set_texto(s_op_lbl[i], buf);
        bool alcanza = tiene(MASCOTA_ACC[i]) || s_e.huesitos >= s_e.precio[i];
        lv_obj_set_style_border_color(s_op[i], lv_color_hex(alcanza ? COLOR_HUESO : 0x444444), 0);
    }
    set_texto(s_op_lbl[MASCOTA_N_ACC], "QUITAR ACCESORIO");
    lv_obj_set_style_border_color(s_op[MASCOTA_N_ACC], lv_color_hex(COLOR_ACCENT), 0);
}

void canela_mascota_tick(float now, bool visible)
{
    if (!s_tile) return;
    if (!visible) {
        if (s_menu != M_NADA) mostrar_menu(M_NADA);
        return;
    }
    canela_mascota_pedir(60);
    static char crudo[1024];
    int edad = 0;
    bool fallo = false, pend = false;
    static int64_t visto_us;
    if (canela_mascota_leer(crudo, sizeof(crudo), &edad, &fallo, &pend)) {
        leer_estado(crudo);
        /* Llegó una respuesta nueva (la de una acción trae su aviso como frase): manda ya. */
        int64_t llego = esp_timer_get_time() - (int64_t)edad * 1000000;
        if (llego > visto_us + 1500000) {
            if (visto_us) s_aviso_hasta = 0;
            visto_us = llego;
        }
    } else if (fallo && !s_hay_estado) {
        set_texto(s_frase, "SIN CONEXION");
    }
    if (!s_hay_estado) {
        if (s_cara < 0) {
            s_cara = 2;
            pintar(2, 0, 5, -1);
        }
        return;
    }

    int cara = cara_indice(s_e.cara);
    bool dormida = !strcmp(s_e.cara, "dormida");
    float periodo = dormida ? 1e9f : (s_e.paseando >= 0 || s_e.animo >= 70) ? 0.25f : s_e.animo >= 40 ? 0.5f : 1.0f;
    int cuadro = dormida ? 0 : (int)(now / periodo) % 2;
    int escala = escala_de(s_e.etapa);
    int acc = acc_indice(s_e.puesto);
    if (cara != s_cara || cuadro != s_cuadro || escala != s_escala_pintada || acc != s_acc_pintado) {
        pintar(cara, cuadro, escala, acc);
        s_cara = cara;
        s_cuadro = cuadro;
        s_escala_pintada = escala;
        s_acc_pintado = acc;
    }
    lv_obj_set_flag(s_zzz, LV_OBJ_FLAG_HIDDEN, !dormida);

    char buf[72];
    if (s_e.forma[0]) snprintf(buf, sizeof(buf), "%s %s", s_e.etapa, s_e.forma);
    else if (s_e.tendencia[0]) snprintf(buf, sizeof(buf), "%s - VA PARA %s", s_e.etapa, s_e.tendencia);
    else strlcpy(buf, s_e.etapa, sizeof(buf));
    set_texto(s_etapa, buf);
    if (s_e.nombre[0]) set_texto(s_nombre, s_e.nombre);
    snprintf(buf, sizeof(buf), "%d HUESITOS (+%d HOY)", s_e.huesitos, s_e.huesitos_hoy);
    set_texto(s_huesos, buf);
    if (now >= s_aviso_hasta) set_texto(s_frase, s_e.frase);
    const int valores[3] = { s_e.condicion, s_e.nutricion, s_e.energia };
    for (int i = 0; i < 3; i++) {
        if (lv_bar_get_value(s_barra[i]) != valores[i]) lv_bar_set_value(s_barra[i], valores[i], LV_ANIM_OFF);
    }
    bool paseo = s_e.paseando >= 0;
    set_texto(s_lbl_correa, paseo ? "VOLVI" : "CORREA");
    lv_obj_set_style_border_color(s_btn_correa, lv_color_hex(paseo ? 0x6FCF7A : COLOR_ACCENT), 0);
    if (s_menu == M_TIENDA) pintar_tienda();
    (void)pend;
}
