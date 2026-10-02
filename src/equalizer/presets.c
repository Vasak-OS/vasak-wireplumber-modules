/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <math.h>

#include "presets.h"

static const gdouble frequencies[VASAK_EQ_BANDS] = {
  31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000,
};

static const gchar *const nodes[VASAK_EQ_BANDS] = {
  "eq_31", "eq_63", "eq_125", "eq_250", "eq_500",
  "eq_1k", "eq_2k", "eq_4k", "eq_8k", "eq_16k",
};

/*
 * Los valores son de oído y conservadores: ninguno pasa de +6 dB, que con el
 * preamplificador ya cuesta 6 dB de volumen. Siguen la forma de los perfiles
 * clásicos de los reproductores de escritorio, no sus números, que suelen
 * llegar a +10 y sólo suenan bien con el volumen bajo.
 *
 *                        31   63  125  250  500   1k   2k   4k   8k  16k
 */
static const VasakEqPreset presets[] = {
  { "flat",    {  0,   0,   0,   0,   0,   0,   0,   0,   0,   0 } },
  { "bass",    {  6,   5,   4,   2,   0,   0,   0,   0,   0,   0 } },
  { "treble",  {  0,   0,   0,   0,   0,   1,   2,   4,   5,   6 } },
  { "vocal",   { -3,  -2,  -1,   1,   3,   4,   3,   1,   0,  -1 } },
  { "pop",     { -1,   1,   3,   4,   3,   0,  -1,  -1,   1,   2 } },
  { "rock",    {  5,   4,   2,  -1,  -2,  -1,   2,   3,   4,   4 } },
  { "jazz",    {  3,   2,   1,   2,  -1,  -1,   0,   1,   2,   3 } },
  { "classic", {  0,   0,   0,   0,   0,   0,  -3,  -3,  -3,  -4 } },
};

gdouble
vasak_eq_band_frequency (guint band)
{
  g_return_val_if_fail (band < VASAK_EQ_BANDS, 0);
  return frequencies[band];
}

const gchar *
vasak_eq_band_node (guint band)
{
  g_return_val_if_fail (band < VASAK_EQ_BANDS, NULL);
  return nodes[band];
}

const VasakEqPreset *
vasak_eq_presets (guint *n_presets)
{
  if (n_presets != NULL)
    *n_presets = G_N_ELEMENTS (presets);
  return presets;
}

const VasakEqPreset *
vasak_eq_preset_find (const gchar *id)
{
  if (id == NULL)
    return NULL;
  for (guint i = 0; i < G_N_ELEMENTS (presets); i++)
    if (g_str_equal (presets[i].id, id))
      return &presets[i];
  return NULL;
}

gboolean
vasak_eq_preset_id_valid (const gchar *id)
{
  return g_strcmp0 (id, VASAK_EQ_PRESET_CUSTOM) == 0 ||
         vasak_eq_preset_find (id) != NULL;
}

gboolean
vasak_eq_gain_valid (gdouble gain)
{
  /* `isfinite` antes que el rango: NaN compara falso contra todo, así que
   * `gain >= MIN && gain <= MAX` lo rechazaría igual, pero por casualidad. */
  return isfinite (gain) && gain >= VASAK_EQ_GAIN_MIN &&
         gain <= VASAK_EQ_GAIN_MAX;
}

gdouble
vasak_eq_preamp (const gdouble gains[VASAK_EQ_BANDS])
{
  gdouble highest = 0;
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    if (gains[i] > highest)
      highest = gains[i];
  /* Con ninguna banda arriba, 0 y no `-highest`: `-0.0` viajaría como «-0»
   * por D-Bus y en el archivo. */
  return highest > 0 ? -highest : 0;
}
