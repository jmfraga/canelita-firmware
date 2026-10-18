/*
 * Copyright (c) 2026 Juan Manuel Fraga.
 * SPDX-License-Identifier: Apache-2.0
 *
 * canelita: página de la mascota de bolsillo (canela_mascota.c). Con el candado de la pantalla tomado.
 */
#pragma once
#include <stdbool.h>
#include "lvgl.h"

void canela_mascota_build(lv_obj_t *tile, int w, int h);
/* Una vez por segundo o más; solo pide y pinta mientras la página se ve. */
void canela_mascota_tick(float now, bool visible);
/* Banco (consola «pagina=anotar|mover|minutos|tienda»): abre ese menú para las capturas. */
void canela_mascota_banco(const char *menu);
