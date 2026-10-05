/*
 * Copyright (c) 2026 Juan Manuel Fraga.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Canela — la cara de canelita.
 *
 * Avatar propio que reemplaza al de Muse por el mecanismo oficial del SDK
 * (tools/muse/AVATAR_RECIPE.md): un muse_pixel.c en components/muse/avatar/.
 * Escrito desde cero contra el contrato de muse_pixel.h; NO deriva del avatar
 * Jollybot de esp32/avatar/, que queda fuera de la licencia Apache.
 *
 * Idea: la cara ES un rollo de canela, como el ícono de Canela. La espiral
 * gira mientras piensa, la boca sigue el nivel de voz al hablar, los ojos se
 * abren más al escuchar y parpadea sola en reposo.
 *
 * Se dibuja en la cuadrícula de MUSE_PX_W x MUSE_PX_H (64x64) que exige el SDK
 * y muse_pixel_scale() la amplía a la pantalla por vecino más cercano. Cada
 * celda se evalúa con unas pocas operaciones (círculos, elipses, una atan2)
 * para quedar dentro de los ~10 ms por cuadro del ESP32-S3.
 */
#include "muse_pixel.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* --- Paleta (0xRRGGBB) ---------------------------------------------------- */
#define C_BG        0x000000u   /* negro puro: igual que la UI y, en AMOLED, pixel apagado = batería */
#define C_CINN      0xc97a3au   /* canela: la masa del rollo */
#define C_CINN_LT   0xe89a5cu   /* canela clara: la espiral */
#define C_CINN_DK   0x8a4a22u   /* contorno y sombra */
#define C_INK       0x2a160cu   /* ojos y boca */
#define C_CREAM     0xf6e7cfu   /* brillo de los ojos */
#define C_BLUSH     0xe9826eu   /* mejillas, lengua */
#define C_OK        0x5ec97au
#define C_AMBER     0xd8a13du
#define C_ERR       0xc5524cu

#define TAU 6.28318530718f

static uint16_t s_grid[MUSE_PX_W * MUSE_PX_H];   /* RGB565 nativo */
static int s_size = MUSE_PX_W;                   /* lado en pixeles de pantalla */

static inline uint16_t rgb565(uint32_t c) {
    uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

/* Atenúa un color hacia el fondo; k en 0..1 (1 = color pleno). */
static inline uint32_t dim(uint32_t c, float k) {
    float r = ((c >> 16) & 0xff), g = ((c >> 8) & 0xff), b = (c & 0xff);
    float br = ((C_BG >> 16) & 0xff), bg = ((C_BG >> 8) & 0xff), bb = (C_BG & 0xff);
    uint32_t R = (uint32_t)(br + (r - br) * k), G = (uint32_t)(bg + (g - bg) * k),
             B = (uint32_t)(bb + (b - bb) * k);
    return (R << 16) | (G << 8) | B;
}

static inline float frac(float x) { return x - floorf(x); }

/* ¿(u,v) cae dentro de la elipse centrada en (cx,cy) con semiejes rx,ry? */
static inline int in_ellipse(float u, float v, float cx, float cy, float rx, float ry) {
    if (rx <= 0.f || ry <= 0.f) return 0;
    float a = (u - cx) / rx, b = (v - cy) / ry;
    return a * a + b * b <= 1.f;
}

/* Banda de un arco de circunferencia de radio rad y grosor th alrededor de (cx,cy). */
static inline int on_ring(float u, float v, float cx, float cy, float rad, float th) {
    float d = sqrtf((u - cx) * (u - cx) + (v - cy) * (v - cy));
    return fabsf(d - rad) <= th;
}

uint32_t muse_pixel_accent(muse_mode_t mode) {
    switch (mode) {
    case MUSE_MODE_LISTENING: return C_OK;
    case MUSE_MODE_THINKING:  return C_AMBER;
    case MUSE_MODE_ERROR:     return C_ERR;
    case MUSE_MODE_OFF:       return C_CINN_DK;
    default:                  return C_CINN_LT;
    }
}

void muse_pixel_render(const muse_pose_t *p) {
    const muse_mode_t m = p->mode;
    const float t = p->t, mt = p->mode_t;
    const float level = p->level < 0.f ? 0.f : (p->level > 1.f ? 1.f : p->level);
    const int happy = p->happy > 0.5f;

    /* Radio de la cara; al arrancar el rollo "se enrolla" creciendo. */
    float R = 0.80f;
    if (m == MUSE_MODE_BOOT) R *= fminf(1.f, 0.35f + mt * 0.9f);
    /* Respiración en reposo: un vaivén casi imperceptible. */
    float bob = (m == MUSE_MODE_IDLE) ? 0.012f * sinf(t * 1.6f) : 0.f;

    /* Giro de la espiral: lento siempre, rápido al pensar. */
    float spin = (m == MUSE_MODE_THINKING) ? mt * 0.45f : t * 0.03f;

    /* Parpadeo espontáneo cada ~4.2 s (sólo despierta y sin estar "feliz"). */
    float open = 1.f;
    if (m == MUSE_MODE_IDLE || m == MUSE_MODE_SPEAKING) {
        float ph = fmodf(t, 4.2f);
        if (ph < 0.14f) open = 0.15f;
    }
    if (m == MUSE_MODE_LISTENING) open = 1.15f;              /* atenta */
    if (m == MUSE_MODE_OFF) open = 0.12f;                    /* dormida */

    /* Mirada: al pensar, arriba y a un lado. */
    float lx = 0.f, ly = 0.f;
    if (m == MUSE_MODE_THINKING) { lx = 0.05f; ly = -0.06f; }

    const float dimk = (m == MUSE_MODE_OFF) ? 0.35f : 1.f;

    for (int y = 0; y < MUSE_PX_H; y++) {
        for (int x = 0; x < MUSE_PX_W; x++) {
            float u = (x + 0.5f) / (MUSE_PX_W * 0.5f) - 1.f;
            float v = (y + 0.5f) / (MUSE_PX_H * 0.5f) - 1.f - bob;
            float r = sqrtf(u * u + v * v);
            uint32_t c = C_BG;

            if (r <= R) {
                /* Masa del rollo con su espiral. */
                c = C_CINN;
                if (r > 0.10f && r < R - 0.07f) {
                    float a = atan2f(v, u);
                    float s = a / TAU + r * 2.3f - spin;
                    if (frac(s) < 0.20f) c = C_CINN_LT;
                }
                if (r > R - 0.05f) c = C_CINN_DK;            /* contorno */

                if (m != MUSE_MODE_BOOT || mt > 0.8f) {
                    /* Mejillas. */
                    if (m != MUSE_MODE_ERROR && m != MUSE_MODE_OFF &&
                        (in_ellipse(u, v, -0.47f, 0.17f, 0.09f, 0.05f) ||
                         in_ellipse(u, v, 0.47f, 0.17f, 0.09f, 0.05f)))
                        c = C_BLUSH;

                    /* Ojos. */
                    for (int s = -1; s <= 1; s += 2) {
                        float ex = s * 0.30f + lx, ey = -0.10f + ly;
                        if (m == MUSE_MODE_ERROR) {
                            /* Una "x" pequeña por ojo. */
                            float du = u - ex, dv = v - ey;
                            if (fabsf(du) < 0.08f && fabsf(fabsf(du) - fabsf(dv)) < 0.03f)
                                c = C_INK;
                        } else if (happy) {
                            /* Ojos de alegría: arquitos hacia arriba. */
                            if (on_ring(u, v, ex, ey + 0.05f, 0.085f, 0.028f) && v < ey + 0.05f)
                                c = C_INK;
                        } else if (in_ellipse(u, v, ex, ey, 0.085f, 0.135f * open)) {
                            c = C_INK;
                            if (open > 0.5f && in_ellipse(u, v, ex - 0.03f, ey - 0.05f, 0.032f, 0.032f))
                                c = C_CREAM;
                        }
                    }

                    /* Boca. */
                    if (m == MUSE_MODE_SPEAKING) {
                        /* Abre con el nivel de voz; un leve temblor para que
                         * se mueva aun si el nivel llega plano. */
                        float ry = 0.03f + 0.13f * level + 0.015f * (0.5f + 0.5f * sinf(t * 14.f));
                        float rx = 0.11f + 0.02f * level;
                        if (in_ellipse(u, v, 0.f, 0.29f, rx, ry)) {
                            c = C_INK;
                            if (ry > 0.07f && v > 0.29f + ry * 0.35f &&
                                in_ellipse(u, v, 0.f, 0.29f + ry * 0.55f, rx * 0.6f, ry * 0.45f))
                                c = C_BLUSH;
                        }
                    } else if (m == MUSE_MODE_THINKING || m == MUSE_MODE_LISTENING) {
                        /* Boca pequeña y quieta: está atendiendo. */
                        if (fabsf(v - 0.29f) < 0.025f && fabsf(u) < 0.07f) c = C_INK;
                    } else if (m == MUSE_MODE_ERROR) {
                        if (on_ring(u, v, 0.f, 0.50f, 0.20f, 0.028f) && v < 0.36f && fabsf(u) < 0.15f)
                            c = C_INK;
                    } else if (m != MUSE_MODE_OFF) {
                        /* Sonrisa. */
                        if (on_ring(u, v, 0.f, 0.06f, 0.22f, 0.03f) && v > 0.20f && fabsf(u) < 0.16f)
                            c = C_INK;
                    }
                }
            }
            s_grid[y * MUSE_PX_W + x] = rgb565(dimk < 1.f ? dim(c, dimk) : c);
        }
    }
}

void muse_pixel_set_size(int px) {
    s_size = px > 0 ? px : MUSE_PX_W;
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1) {
    for (int y = y0; y <= y1; y++) {
        int gy = y * MUSE_PX_H / s_size;
        if (gy >= MUSE_PX_H) gy = MUSE_PX_H - 1;
        const uint16_t *src = &s_grid[gy * MUSE_PX_W];
        uint16_t *row = dst + (size_t)(y - y0) * stride_px;
        for (int x = x0; x <= x1; x++) {
            int gx = x * MUSE_PX_W / s_size;
            if (gx >= MUSE_PX_W) gx = MUSE_PX_W - 1;
            row[x - x0] = src[gx];
        }
    }
}
