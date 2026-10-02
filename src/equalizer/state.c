/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>

#include "state.h"

#define GROUP "equalizer"
#define KEY_ENABLED "enabled"
#define KEY_PRESET "preset"
#define KEY_CUSTOM "custom"

static void
set_preset_id (VasakEqState *state, const gchar *id)
{
  g_strlcpy (state->preset, id, sizeof state->preset);
}

void
vasak_eq_state_defaults (VasakEqState *state)
{
  state->enabled = TRUE;
  set_preset_id (state, VASAK_EQ_PRESET_FLAT);
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    state->custom[i] = 0;
}

gboolean
vasak_eq_state_load (VasakEqState *state, const gchar *path, GError **error)
{
  vasak_eq_state_defaults (state);

  g_autoptr (GKeyFile) file = g_key_file_new ();
  g_autoptr (GError) local = NULL;
  if (!g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, &local)) {
    if (g_error_matches (local, G_FILE_ERROR, G_FILE_ERROR_NOENT))
      return TRUE;
    g_propagate_error (error, g_steal_pointer (&local));
    return FALSE;
  }

  /* Cada campo por su lado: el que falla se queda con lo de fábrica, y no se
   * lleva puestos a los demás. */
  gboolean enabled = g_key_file_get_boolean (file, GROUP, KEY_ENABLED, &local);
  if (local == NULL)
    state->enabled = enabled;
  g_clear_error (&local);

  g_autofree gchar *preset = g_key_file_get_string (file, GROUP, KEY_PRESET,
                                                    NULL);
  if (vasak_eq_preset_id_valid (preset))
    set_preset_id (state, preset);

  gsize n = 0;
  g_autofree gdouble *custom = g_key_file_get_double_list (file, GROUP,
                                                           KEY_CUSTOM, &n,
                                                           NULL);
  /* Una lista de otro largo no se recorta ni se rellena: no hay forma de saber
   * a qué banda iba cada número. */
  if (custom != NULL && n == VASAK_EQ_BANDS)
    for (guint i = 0; i < VASAK_EQ_BANDS; i++)
      state->custom[i] = vasak_eq_gain_valid (custom[i]) ? custom[i] : 0;

  return TRUE;
}

gboolean
vasak_eq_state_save (const VasakEqState *state, const gchar *path,
                     GError **error)
{
  g_autoptr (GKeyFile) file = g_key_file_new ();
  g_key_file_set_comment (file, NULL, NULL,
      " Estado del ecualizador de VasakOS. Lo escribe el módulo de WirePlumber\n"
      " cada vez que se cambia algo desde el escritorio o Ajustes.",
      NULL);
  g_key_file_set_boolean (file, GROUP, KEY_ENABLED, state->enabled);
  g_key_file_set_string (file, GROUP, KEY_PRESET, state->preset);
  /* Una copia y no un cast: `g_key_file_set_double_list` pide un puntero sin
   * `const` aunque no escribe, y sacarle el `const` a `state` sería mentirle
   * al compilador sobre quién puede tocarlo. */
  gdouble custom[VASAK_EQ_BANDS];
  memcpy (custom, state->custom, sizeof custom);
  g_key_file_set_double_list (file, GROUP, KEY_CUSTOM, custom, VASAK_EQ_BANDS);

  g_autofree gchar *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0) {
    int saved = errno;
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved),
                 "no se pudo crear %s: %s", dir, g_strerror (saved));
    return FALSE;
  }

  gsize length = 0;
  g_autofree gchar *data = g_key_file_to_data (file, &length, NULL);
  /* `g_file_set_contents` escribe en un temporal y renombra: lo que hay en
   * disco es siempre un archivo entero, el viejo o el nuevo. */
  return g_file_set_contents (path, data, (gssize) length, error);
}

gchar *
vasak_eq_state_default_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "vasak", "equalizer.ini",
                           NULL);
}

void
vasak_eq_state_gains (const VasakEqState *state, gdouble out[VASAK_EQ_BANDS])
{
  const VasakEqPreset *preset = vasak_eq_preset_find (state->preset);
  const gdouble *source = preset != NULL ? preset->gains : state->custom;
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    out[i] = source[i];
}

/* Lo que suena pasa a ser el perfil propio, sin cambiar lo que se oye. */
static void
become_custom (VasakEqState *state)
{
  gdouble current[VASAK_EQ_BANDS];
  vasak_eq_state_gains (state, current);
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    state->custom[i] = current[i];
  set_preset_id (state, VASAK_EQ_PRESET_CUSTOM);
}

gboolean
vasak_eq_state_set_gain (VasakEqState *state, guint band, gdouble gain)
{
  if (band >= VASAK_EQ_BANDS || !vasak_eq_gain_valid (gain))
    return FALSE;
  become_custom (state);
  state->custom[band] = gain;
  return TRUE;
}

gboolean
vasak_eq_state_set_gains (VasakEqState *state,
                          const gdouble gains[VASAK_EQ_BANDS])
{
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    if (!vasak_eq_gain_valid (gains[i]))
      return FALSE;
  set_preset_id (state, VASAK_EQ_PRESET_CUSTOM);
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    state->custom[i] = gains[i];
  return TRUE;
}

gboolean
vasak_eq_state_set_preset (VasakEqState *state, const gchar *id)
{
  if (!vasak_eq_preset_id_valid (id))
    return FALSE;
  set_preset_id (state, id);
  return TRUE;
}

gboolean
vasak_eq_state_equal (const VasakEqState *a, const VasakEqState *b)
{
  if (a->enabled != b->enabled || g_strcmp0 (a->preset, b->preset) != 0)
    return FALSE;
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    if (a->custom[i] != b->custom[i])
      return FALSE;
  return TRUE;
}
