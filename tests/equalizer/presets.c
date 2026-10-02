/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <math.h>

#include "presets.h"

static void
test_bands (void)
{
  /* Las del video de referencia, en este orden: la interfaz las dibuja por
   * índice. */
  static const gdouble expected[] = { 31, 63, 125, 250, 500,
                                      1000, 2000, 4000, 8000, 16000 };
  g_assert_cmpuint (G_N_ELEMENTS (expected), ==, VASAK_EQ_BANDS);
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    g_assert_cmpfloat (vasak_eq_band_frequency (i), ==, expected[i]);

  g_assert_cmpstr (vasak_eq_band_node (0), ==, "eq_31");
  g_assert_cmpstr (vasak_eq_band_node (5), ==, "eq_1k");
  g_assert_cmpstr (vasak_eq_band_node (9), ==, "eq_16k");
}

static void
test_band_out_of_range (void)
{
  if (g_test_subprocess ()) {
    g_assert_null (vasak_eq_band_node (VASAK_EQ_BANDS));
    return;
  }
  /* `g_return_val_if_fail` avisa con un crítico: que lo haga, y que no se
   * salga de la tabla. */
  g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_stderr ("*CRITICAL*band < VASAK_EQ_BANDS*");
}

static void
test_presets_order (void)
{
  static const gchar *expected[] = { "flat", "bass", "treble", "vocal",
                                     "pop", "rock", "jazz", "classic" };
  guint n = 0;
  const VasakEqPreset *presets = vasak_eq_presets (&n);
  g_assert_cmpuint (n, ==, G_N_ELEMENTS (expected));
  for (guint i = 0; i < n; i++) {
    g_assert_cmpstr (presets[i].id, ==, expected[i]);
    /* Ninguno se sale del rango que se acepta por D-Bus, ni pasa de +6: el
     * preamplificador le cobra eso al volumen. */
    for (guint b = 0; b < VASAK_EQ_BANDS; b++) {
      g_assert_true (vasak_eq_gain_valid (presets[i].gains[b]));
      g_assert_cmpfloat (presets[i].gains[b], <=, 6);
    }
  }
}

static void
test_flat_is_flat (void)
{
  const VasakEqPreset *flat = vasak_eq_preset_find ("flat");
  g_assert_nonnull (flat);
  for (guint b = 0; b < VASAK_EQ_BANDS; b++)
    g_assert_cmpfloat (flat->gains[b], ==, 0);
  g_assert_cmpfloat (vasak_eq_preamp (flat->gains), ==, 0);
}

static void
test_find_and_valid (void)
{
  g_assert_nonnull (vasak_eq_preset_find ("rock"));
  g_assert_null (vasak_eq_preset_find ("Rock"));
  g_assert_null (vasak_eq_preset_find (NULL));
  /* «custom» se elige pero no es de fábrica: no tiene valores en la tabla. */
  g_assert_null (vasak_eq_preset_find ("custom"));
  g_assert_true (vasak_eq_preset_id_valid ("custom"));
  g_assert_true (vasak_eq_preset_id_valid ("classic"));
  g_assert_false (vasak_eq_preset_id_valid ("saved"));
  g_assert_false (vasak_eq_preset_id_valid (""));
  g_assert_false (vasak_eq_preset_id_valid (NULL));
}

static void
test_gain_valid (void)
{
  g_assert_true (vasak_eq_gain_valid (0));
  g_assert_true (vasak_eq_gain_valid (VASAK_EQ_GAIN_MIN));
  g_assert_true (vasak_eq_gain_valid (VASAK_EQ_GAIN_MAX));
  g_assert_false (vasak_eq_gain_valid (VASAK_EQ_GAIN_MAX + 0.01));
  g_assert_false (vasak_eq_gain_valid (VASAK_EQ_GAIN_MIN - 0.01));
  g_assert_false (vasak_eq_gain_valid (NAN));
  g_assert_false (vasak_eq_gain_valid (INFINITY));
  g_assert_false (vasak_eq_gain_valid (-INFINITY));
}

static void
test_preamp (void)
{
  gdouble gains[VASAK_EQ_BANDS] = { 0 };
  g_assert_cmpfloat (vasak_eq_preamp (gains), ==, 0);
  /* Y no -0: viajaría así por D-Bus. */
  g_assert_false (signbit (vasak_eq_preamp (gains)));

  gains[1] = 6;
  gains[7] = 3.5;
  g_assert_cmpfloat (vasak_eq_preamp (gains), ==, -6);

  /* Lo que baja no cuenta: bajar una banda no satura. */
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    gains[i] = -4;
  g_assert_cmpfloat (vasak_eq_preamp (gains), ==, 0);

  const VasakEqPreset *rock = vasak_eq_preset_find ("rock");
  g_assert_cmpfloat (vasak_eq_preamp (rock->gains), ==, -5);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/presets/las diez bandas del video, en orden", test_bands);
  g_test_add_func ("/presets/una banda fuera de la tabla avisa y no lee",
                   test_band_out_of_range);
  g_test_add_func ("/presets/los ocho perfiles, en el orden de la grilla",
                   test_presets_order);
  g_test_add_func ("/presets/plano es plano y no gasta margen",
                   test_flat_is_flat);
  g_test_add_func ("/presets/buscar y validar ids", test_find_and_valid);
  g_test_add_func ("/presets/la ganancia válida es finita y en rango",
                   test_gain_valid);
  g_test_add_func ("/presets/el preamplificador baja lo que sube la más alta",
                   test_preamp);
  return g_test_run ();
}
