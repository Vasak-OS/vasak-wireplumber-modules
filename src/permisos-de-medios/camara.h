/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib.h>

/**
 * Si un objeto de PipeWire es una cámara, a los efectos del permiso.
 *
 * Cualquiera de los argumentos puede ser NULL — un objeto que no tiene esa
 * propiedad—, y eso no es un error.
 *
 * Va aparte de la parte que habla con WirePlumber para que se pueda probar sin
 * un servidor andando: acá es donde se decide qué se oculta, y una regla de
 * más deja al escritorio sin compartir pantalla mientras que una de menos deja
 * la cámara abierta. Las dos se ven igual de bien mirando el código.
 */
gboolean vasak_medios_es_camara (const gchar *media_class,
                                 const gchar *media_role,
                                 const gchar *device_api);
