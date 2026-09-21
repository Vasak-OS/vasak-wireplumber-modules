/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib.h>
#include <sys/types.h>

/**
 * Lee el instante de arranque de un proceso desde el contenido de
 * `/proc/<pid>/stat`, que es el campo 22.
 *
 * Va aparte y toma la cadena en vez del pid para que se pueda probar sin un
 * proceso de verdad: el formato de ese archivo tiene una trampa —ver la
 * implementación— y equivocarse ahí no falla, devuelve otro número.
 *
 * Devuelve FALSE si la cadena no tiene la forma esperada. No escribe `inicio`
 * en ese caso.
 */
gboolean vasak_medios_inicio_desde_stat (const gchar *stat, guint64 *inicio);

/**
 * Lo mismo, pero leyendo `/proc/<pid>/stat`. Devuelve FALSE si el proceso ya
 * no está o el archivo no se puede leer.
 */
gboolean vasak_medios_inicio_del_proceso (pid_t pid, guint64 *inicio);
