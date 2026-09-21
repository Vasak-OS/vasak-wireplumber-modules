/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "identidad.h"

#include <stdlib.h>
#include <string.h>

/*
 * ── La trampa de /proc/<pid>/stat ───────────────────────────────────────────
 *
 * El campo 2 es el nombre del ejecutable entre paréntesis, y **puede contener
 * espacios y paréntesis**: el kernel no los escapa. Un proceso llamado
 * `mi (raro) programa` produce
 *
 *     1234 (mi (raro) programa) S 1 1234 ...
 *
 * así que partir por espacios, o buscar el primer `)`, da un número
 * equivocado. Y equivocarse acá **no falla**: devuelve otro entero, el
 * servicio de permisos lo rechaza como si el proceso hubiera muerto, y la
 * cámara queda negada sin explicación.
 *
 * La forma correcta, que es la que documenta proc(5): buscar el **último**
 * `)`. Desde ahí, el siguiente campo es el 3, así que el 22 —`starttime`— es
 * el vigésimo.
 */
#define CAMPO_STARTTIME 22

gboolean
vasak_medios_inicio_desde_stat (const gchar *stat, guint64 *inicio)
{
  g_return_val_if_fail (inicio != NULL, FALSE);

  if (stat == NULL)
    return FALSE;

  const gchar *cierre = strrchr (stat, ')');
  if (cierre == NULL)
    return FALSE;

  /* El campo 3 empieza después del `)` y del espacio que lo sigue. */
  const gchar *p = cierre + 1;

  /* Desde el campo 3 hasta el 22 hay que saltar los de en medio. */
  for (int campo = 3; campo < CAMPO_STARTTIME; campo++) {
    while (*p == ' ')
      p++;
    if (*p == '\0' || *p == '\n')
      return FALSE;
    while (*p != ' ' && *p != '\0' && *p != '\n')
      p++;
  }

  while (*p == ' ')
    p++;
  if (*p < '0' || *p > '9')
    return FALSE;

  gchar *fin = NULL;
  guint64 valor = g_ascii_strtoull (p, &fin, 10);
  if (fin == p)
    return FALSE;

  *inicio = valor;
  return TRUE;
}

gboolean
vasak_medios_inicio_del_proceso (pid_t pid, guint64 *inicio)
{
  g_return_val_if_fail (inicio != NULL, FALSE);

  g_autofree gchar *ruta = g_strdup_printf ("/proc/%d/stat", (int) pid);
  g_autofree gchar *contenido = NULL;

  if (!g_file_get_contents (ruta, &contenido, NULL, NULL))
    return FALSE;

  return vasak_medios_inicio_desde_stat (contenido, inicio);
}
