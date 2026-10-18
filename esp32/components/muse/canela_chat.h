/*
 * Copyright (c) 2026 Juan Manuel Fraga.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Comandos de consola canelita.* (configuración por USB). true si la línea era
 * de canelita y ya se atendió. Ver canela_chat.c. */
bool canela_console(char *line);

#include <stddef.h>

/* Agenda de las próximas 24 h para la pantalla del reloj (GET /v1/agentes/canela/agenda), ya en ASCII.
 * pedir() la encarga a la tarea de red si la última tiene más de `edad_max_s`; leer() copia la última que llegó
 * (renglones `HH:MM<TAB>título`, `MANANA` separa) y dice hace cuántos segundos y si el último intento falló. */
void canela_agenda_pedir(int edad_max_s);
bool canela_agenda_leer(char *out, size_t cap, int *edad_s, bool *fallo);

/* Mascota de bolsillo (GET/POST /v1/agentes/canela/mascota): renglones `clave<TAB>valor` en ASCII.
 * pedir() la renueva si la última tiene más de `edad_max_s`; accion() encarga «comer», «jugar» o «ejercicio»
 * (con minutos) y es false si ya había una petición en camino; leer() copia el último estado. */
void canela_mascota_pedir(int edad_max_s);
bool canela_mascota_accion(const char *accion, int minutos);
bool canela_mascota_leer(char *out, size_t cap, int *edad_s, bool *fallo, bool *pendiente);

/* Con quién habla el botón: «canela» o «vera» (Vera 2, página del colgante). Lo fija la página activa al
 * apretar; los turnos y la llamada van a /v1/agentes/<agente>/. Las notas guardadas sin red son solo de Canela. */
void canela_agente_usar(const char *agente);
const char *canela_agente(void);
const char *canela_agente_nombre(void);   /* para la pantalla: «CANELA» o «VERA 2» */

/* Imagen de Vera 2 para su página (200x200 RGB565): NULL mientras no ha llegado (y la encarga); luego el
 * búfer en PSRAM. Se baja autenticada de canela-web: no viaja en el firmware ni en ningún repo. */
const uint8_t *canela_vera_imagen(void);
