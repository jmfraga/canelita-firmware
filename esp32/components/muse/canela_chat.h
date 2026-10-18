/*
 * Copyright (c) 2026 Juan Manuel Fraga.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>

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
