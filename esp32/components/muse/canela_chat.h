/*
 * Copyright (c) 2026 Juan Manuel Fraga.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>

/* Comandos de consola canelita.* (configuración por USB). true si la línea era
 * de canelita y ya se atendió. Ver canela_chat.c. */
bool canela_console(char *line);
