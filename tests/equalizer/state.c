/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#include "state.h"

typedef struct {
  gchar *dir;
  gchar *path;
} Fixture;

static void
setup (Fixture *f, gconstpointer data G_GNUC_UNUSED)
{
  f->dir = g_dir_make_tmp ("vasak-eq-state-XXXXXX", NULL);
  g_assert_nonnull (f->dir);
  /* Un subdirectorio que no existe: guardar lo tiene que crear. */
  f->path = g_build_filename (f->dir, "vasak", "equalizer.ini", NULL);
}

static void
teardown (Fixture *f, gconstpointer data G_GNUC_UNUSED)
{
  g_autofree gchar *sub = g_path_get_dirname (f->path);
  g_remove (f->path);
  g_rmdir (sub);
  g_rmdir (f->dir);
  g_free (f->path);
  g_free (f->dir);
}

static void
write_file (Fixture *f, const gchar *contents)
{
  g_autofree gchar *sub = g_path_get_dirname (f->path);
  g_assert_cmpint (g_mkdir_with_parents (sub, 0700), ==, 0);
  g_assert_true (g_file_set_contents (f->path, contents, -1, NULL));
}

static void
assert_gains (const VasakEqState *state, const gdouble expected[VASAK_EQ_BANDS])
{
  gdouble gains[VASAK_EQ_BANDS];
  vasak_eq_state_gains (state, gains);
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    g_assert_cmpfloat (gains[i], ==, expected[i]);
}

static void
test_defaults (void)
{
  VasakEqState s;
  vasak_eq_state_defaults (&s);
  g_assert_true (s.enabled);
  g_assert_cmpstr (s.preset, ==, "flat");
  static const gdouble zero[VASAK_EQ_BANDS] = { 0 };
  assert_gains (&s, zero);
}

static void
test_editing_a_preset_becomes_custom (void)
{
  VasakEqState s;
  vasak_eq_state_defaults (&s);
  g_assert_true (vasak_eq_state_set_preset (&s, "rock"));
  const VasakEqPreset *rock = vasak_eq_preset_find ("rock");
  assert_gains (&s, rock->gains);

  /* Se mueve una banda de «rock»: el propio pasa a ser «rock» con esa banda
   * movida, que es lo que la persona ve. No vuelve a cero lo demás. */
  g_assert_true (vasak_eq_state_set_gain (&s, 3, 2.5));
  g_assert_cmpstr (s.preset, ==, "custom");
  gdouble expected[VASAK_EQ_BANDS];
  memcpy (expected, rock->gains, sizeof expected);
  expected[3] = 2.5;
  assert_gains (&s, expected);

  /* Ir a otro perfil y volver al propio recupera lo propio. */
  g_assert_true (vasak_eq_state_set_preset (&s, "jazz"));
  assert_gains (&s, vasak_eq_preset_find ("jazz")->gains);
  g_assert_true (vasak_eq_state_set_preset (&s, "custom"));
  assert_gains (&s, expected);
}

static void
test_invalid_changes_change_nothing (void)
{
  VasakEqState s, before;
  vasak_eq_state_defaults (&s);
  g_assert_true (vasak_eq_state_set_preset (&s, "pop"));
  before = s;

  g_assert_false (vasak_eq_state_set_gain (&s, VASAK_EQ_BANDS, 1));
  g_assert_false (vasak_eq_state_set_gain (&s, 0, 12.5));
  g_assert_false (vasak_eq_state_set_gain (&s, 0, NAN));
  g_assert_false (vasak_eq_state_set_preset (&s, "loudness"));

  gdouble gains[VASAK_EQ_BANDS] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 };
  gains[9] = -13;
  g_assert_false (vasak_eq_state_set_gains (&s, gains));

  /* Nada a medias: sigue en «pop», y el propio no se tocó. */
  g_assert_true (vasak_eq_state_equal (&s, &before));
}

static void
test_set_gains (void)
{
  VasakEqState s;
  vasak_eq_state_defaults (&s);
  gdouble gains[VASAK_EQ_BANDS] = { 1, -1, 2, -2, 3, -3, 4, -4, 5, -5 };
  g_assert_true (vasak_eq_state_set_gains (&s, gains));
  g_assert_cmpstr (s.preset, ==, "custom");
  assert_gains (&s, gains);
}

static void
test_missing_file_is_first_boot (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  VasakEqState s;
  g_autoptr (GError) error = NULL;
  g_assert_true (vasak_eq_state_load (&s, f->path, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (s.preset, ==, "flat");
  g_assert_true (s.enabled);
}

static void
test_round_trip (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  VasakEqState s, loaded;
  vasak_eq_state_defaults (&s);
  gdouble gains[VASAK_EQ_BANDS] = { 0.5, -1.25, 2, 0, 0, 0, 0, 0, 3, -12 };
  g_assert_true (vasak_eq_state_set_gains (&s, gains));
  g_assert_true (vasak_eq_state_set_preset (&s, "classic"));
  s.enabled = FALSE;

  g_autoptr (GError) error = NULL;
  g_assert_true (vasak_eq_state_save (&s, f->path, &error));
  g_assert_no_error (error);
  g_assert_true (vasak_eq_state_load (&loaded, f->path, &error));
  g_assert_no_error (error);

  /* Todo vuelve: el perfil elegido, el apagado, y el propio aunque no sea el
   * que suena. */
  g_assert_true (vasak_eq_state_equal (&s, &loaded));
  g_assert_cmpstr (loaded.preset, ==, "classic");
  g_assert_false (loaded.enabled);
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    g_assert_cmpfloat (loaded.custom[i], ==, gains[i]);
}

static void
test_bad_fields_fall_back_one_by_one (Fixture *f,
                                      gconstpointer d G_GNUC_UNUSED)
{
  /* El perfil no existe y una banda está fuera de rango; «enabled» y las
   * otras nueve bandas sí se entienden. */
  write_file (f,
      "[equalizer]\n"
      "enabled=false\n"
      "preset=loudness\n"
      "custom=1;2;99;4;5;6;7;8;9;10\n");
  VasakEqState s;
  g_assert_true (vasak_eq_state_load (&s, f->path, NULL));
  g_assert_false (s.enabled);
  g_assert_cmpstr (s.preset, ==, "flat");
  g_assert_cmpfloat (s.custom[0], ==, 1);
  g_assert_cmpfloat (s.custom[2], ==, 0);
  g_assert_cmpfloat (s.custom[9], ==, 10);
}

static void
test_wrong_length_is_ignored (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  write_file (f,
      "[equalizer]\n"
      "preset=custom\n"
      "custom=6;6;6\n");
  VasakEqState s;
  g_assert_true (vasak_eq_state_load (&s, f->path, NULL));
  g_assert_cmpstr (s.preset, ==, "custom");
  for (guint i = 0; i < VASAK_EQ_BANDS; i++)
    g_assert_cmpfloat (s.custom[i], ==, 0);
}

static void
test_garbage_is_an_error_but_boots (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  write_file (f, "esto no es un archivo de claves\n\x01\x02");
  VasakEqState s;
  g_autoptr (GError) error = NULL;
  g_assert_false (vasak_eq_state_load (&s, f->path, &error));
  g_assert_nonnull (error);
  /* Y arranca igual, plano y encendido. */
  g_assert_cmpstr (s.preset, ==, "flat");
  g_assert_true (s.enabled);
}

static void
test_default_path (void)
{
  g_autofree gchar *path = vasak_eq_state_default_path ();
  g_autofree gchar *expected = g_build_filename (g_get_user_state_dir (),
                                                 "vasak", "equalizer.ini",
                                                 NULL);
  g_assert_cmpstr (path, ==, expected);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/state/lo de fábrica es plano y encendido", test_defaults);
  g_test_add_func ("/state/mover una banda de un perfil lo vuelve propio",
                   test_editing_a_preset_becomes_custom);
  g_test_add_func ("/state/un cambio inválido no cambia nada",
                   test_invalid_changes_change_nothing);
  g_test_add_func ("/state/las diez de una vez", test_set_gains);
  g_test_add_func ("/state/la ruta va en XDG_STATE_HOME", test_default_path);
  g_test_add ("/state/sin archivo es el primer arranque", Fixture, NULL,
              setup, test_missing_file_is_first_boot, teardown);
  g_test_add ("/state/lo guardado vuelve entero", Fixture, NULL,
              setup, test_round_trip, teardown);
  g_test_add ("/state/un campo roto no se lleva a los demás", Fixture, NULL,
              setup, test_bad_fields_fall_back_one_by_one, teardown);
  g_test_add ("/state/una lista de otro largo se descarta", Fixture, NULL,
              setup, test_wrong_length_is_ignored, teardown);
  g_test_add ("/state/un archivo ilegible avisa y arranca plano", Fixture,
              NULL, setup, test_garbage_is_an_error_but_boots, teardown);
  return g_test_run ();
}
