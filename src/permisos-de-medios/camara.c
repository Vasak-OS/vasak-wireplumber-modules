/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "camara.h"

/*
 * Los dos objetos que hay que cubrir, y por qué son dos:
 *
 *   · el **nodo** `Video/Source` con `media.role = Camera`, que es lo que se
 *     enlaza para capturar. El rol lo pone PipeWire al crear el nodo desde el
 *     dispositivo; un flujo de captura de pantalla es `Stream/Output/Video` y
 *     no lleva rol de cámara, así que compartir pantalla no se toca.
 *
 *   · el **dispositivo** `Video/Device` con `device.api = v4l2`, que no hace
 *     falta para capturar pero enumera las cámaras del equipo con marca y
 *     modelo. Acá cae también el bucle v4l2 que publica `vasak-connect` con la
 *     cámara del teléfono, que es una cámara igual que las otras.
 *
 * Se exige `device.api` en el dispositivo a propósito: `Video/Device` sin esa
 * propiedad puede ser cualquier otra cosa, y negar de más rompe callado.
 */

gboolean
vasak_medios_es_camara (const gchar *media_class,
                        const gchar *media_role,
                        const gchar *device_api)
{
  if (media_class == NULL)
    return FALSE;

  if (g_strcmp0 (media_class, "Video/Source") == 0)
    return g_strcmp0 (media_role, "Camera") == 0;

  if (g_strcmp0 (media_class, "Video/Device") == 0)
    return g_strcmp0 (device_api, "v4l2") == 0;

  return FALSE;
}
