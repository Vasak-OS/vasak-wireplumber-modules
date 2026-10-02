/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * La interfaz D-Bus, vista desde un cliente de verdad sobre un bus privado.
 *
 * Es el contrato con la interfaz gráfica (vasak-desktop, vasak-settings): lo
 * que se prueba acá es lo que el README promete.
 */

#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#include "service.h"

typedef struct {
  GTestDBus *bus;
  GDBusConnection *server;
  GDBusConnection *client;
  VasakEqService *service;
  gchar *dir;
  gchar *path;

  guint applied;
  VasakEqState last_applied;

  /* Lo último que llegó por `PropertiesChanged`. */
  GVariant *changed;
  guint signals;
  guint subscription;
} Fixture;

static void
on_apply (const VasakEqState *state, gpointer data)
{
  Fixture *f = data;
  f->applied++;
  f->last_applied = *state;
}

static void
on_properties_changed (GDBusConnection *c G_GNUC_UNUSED,
                       const gchar *sender G_GNUC_UNUSED,
                       const gchar *path G_GNUC_UNUSED,
                       const gchar *iface G_GNUC_UNUSED,
                       const gchar *signal G_GNUC_UNUSED,
                       GVariant *parameters, gpointer data)
{
  Fixture *f = data;
  g_clear_pointer (&f->changed, g_variant_unref);
  f->changed = g_variant_get_child_value (parameters, 1);
  f->signals++;
}

static void
setup (Fixture *f, gconstpointer data G_GNUC_UNUSED)
{
  f->dir = g_dir_make_tmp ("vasak-eq-service-XXXXXX", NULL);
  f->path = g_build_filename (f->dir, "equalizer.ini", NULL);

  f->bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (f->bus);
  const gchar *address = g_test_dbus_get_bus_address (f->bus);
  GDBusConnectionFlags flags = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION;
  f->server = g_dbus_connection_new_for_address_sync (address, flags, NULL,
                                                      NULL, NULL);
  f->client = g_dbus_connection_new_for_address_sync (address, flags, NULL,
                                                      NULL, NULL);
  g_assert_nonnull (f->server);
  g_assert_nonnull (f->client);

  /* Guardar «enseguida»: 0 ms es la próxima vuelta del bucle. */
  f->service = vasak_eq_service_new (f->path, 0, on_apply, f);
  g_assert_true (vasak_eq_service_export (f->service, f->server, NULL));

  f->subscription = g_dbus_connection_signal_subscribe (
      f->client, g_dbus_connection_get_unique_name (f->server),
      "org.freedesktop.DBus.Properties", "PropertiesChanged",
      VASAK_EQ_OBJECT_PATH, VASAK_EQ_INTERFACE, G_DBUS_SIGNAL_FLAGS_NONE,
      on_properties_changed, f, NULL);
}

static void
teardown (Fixture *f, gconstpointer data G_GNUC_UNUSED)
{
  g_dbus_connection_signal_unsubscribe (f->client, f->subscription);
  vasak_eq_service_free (f->service);
  g_clear_pointer (&f->changed, g_variant_unref);
  g_object_unref (f->client);
  g_object_unref (f->server);
  g_test_dbus_down (f->bus);
  g_object_unref (f->bus);
  g_remove (f->path);
  g_rmdir (f->dir);
  g_free (f->path);
  g_free (f->dir);
}

/* Que corra el bucle hasta que no quede nada pendiente, con un tope. */
static void
spin (void)
{
  for (guint i = 0; i < 20; i++) {
    while (g_main_context_iteration (NULL, FALSE))
      ;
    g_usleep (1000);
  }
}

/*
 * Las llamadas van asíncronas y se espera iterando el bucle: el servicio
 * contesta desde el mismo hilo, y una llamada síncrona lo dejaría sin poder
 * contestar hasta que venza la espera.
 */
typedef struct {
  GVariant *reply;
  GError *error;
  gboolean done;
} Pending;

static void
on_reply (GObject *source, GAsyncResult *res, gpointer data)
{
  Pending *p = data;
  p->reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res,
                                            &p->error);
  p->done = TRUE;
}

/*
 * Que hayan llegado al menos `n` avisos, esperando hasta 5 s. Los avisos pasan
 * por el demonio del bus, y con la máquina cargada tardan más que una vuelta
 * del bucle: contar enseguida da un rojo que no es del código.
 */
static void
wait_signals (Fixture *f, guint n)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while (f->signals < n && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, FALSE);
  /* Y un rato más, para que un aviso de más —que sería un error— también
   * llegue y se cuente. */
  spin ();
}

static GVariant *
call_full (Fixture *f, const gchar *iface, const gchar *method, GVariant *args,
           GError **error)
{
  Pending p = { 0 };
  g_dbus_connection_call (f->client,
                          g_dbus_connection_get_unique_name (f->server),
                          VASAK_EQ_OBJECT_PATH, iface, method, args, NULL,
                          G_DBUS_CALL_FLAGS_NONE, 2000, NULL, on_reply, &p);
  while (!p.done)
    g_main_context_iteration (NULL, TRUE);
  /* Y lo que haya quedado atrás: el guardado de 0 ms y los avisos. */
  spin ();
  if (p.error != NULL)
    g_propagate_error (error, p.error);
  return p.reply;
}

static GVariant *
call (Fixture *f, const gchar *method, GVariant *args, GError **error)
{
  return call_full (f, VASAK_EQ_INTERFACE, method, args, error);
}

static GVariant *
get (Fixture *f, const gchar *property)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) r = call_full (
      f, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new ("(ss)", VASAK_EQ_INTERFACE, property), &error);
  g_assert_no_error (error);
  GVariant *v = NULL;
  g_variant_get (r, "(v)", &v);
  return v;
}

static void
assert_doubles (GVariant *v, const gdouble *expected, gsize n)
{
  gsize len = 0;
  const gdouble *values = g_variant_get_fixed_array (v, &len, sizeof (gdouble));
  g_assert_cmpuint (len, ==, n);
  for (gsize i = 0; i < n; i++)
    g_assert_cmpfloat (values[i], ==, expected[i]);
}

static gboolean
changed_has (Fixture *f, const gchar *property)
{
  if (f->changed == NULL)
    return FALSE;
  g_autoptr (GVariant) value = g_variant_lookup_value (f->changed, property,
                                                       NULL);
  return value != NULL;
}

static void
test_read_only_description (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  g_autoptr (GVariant) freqs = get (f, "Frequencies");
  static const gdouble expected[] = { 31, 63, 125, 250, 500,
                                      1000, 2000, 4000, 8000, 16000 };
  assert_doubles (freqs, expected, G_N_ELEMENTS (expected));

  g_autoptr (GVariant) range = get (f, "GainRange");
  gdouble min, max;
  g_variant_get (range, "(dd)", &min, &max);
  g_assert_cmpfloat (min, ==, -12);
  g_assert_cmpfloat (max, ==, 12);

  g_autoptr (GVariant) presets = get (f, "Presets");
  g_autofree const gchar **ids = g_variant_get_strv (presets, NULL);
  g_assert_cmpuint (g_strv_length ((gchar **) ids), ==, 8);
  g_assert_cmpstr (ids[0], ==, "flat");
  g_assert_cmpstr (ids[7], ==, "classic");
}

static void
test_initial_state (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  g_autoptr (GVariant) preset = get (f, "Preset");
  g_assert_cmpstr (g_variant_get_string (preset, NULL), ==, "flat");
  g_autoptr (GVariant) enabled = get (f, "Enabled");
  g_assert_true (g_variant_get_boolean (enabled));
  /* Nadie dijo todavía que el nodo exista. */
  g_autoptr (GVariant) available = get (f, "Available");
  g_assert_false (g_variant_get_boolean (available));
  /* Sin archivo, lo de fábrica es lo que va a sonar después de reiniciar. */
  g_autoptr (GVariant) saved = get (f, "Saved");
  g_assert_true (g_variant_get_boolean (saved));
}

static void
test_set_preset (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) r = call (f, "SetPreset", g_variant_new ("(s)", "rock"),
                                 &error);
  g_assert_no_error (error);

  /* Sonó: se le pidió al módulo que aplicara «rock». */
  g_assert_cmpuint (f->applied, ==, 1);
  g_assert_cmpstr (f->last_applied.preset, ==, "rock");

  g_autoptr (GVariant) gains = get (f, "Gains");
  static const gdouble rock[] = { 5, 4, 2, -1, -2, -1, 2, 3, 4, 4 };
  assert_doubles (gains, rock, G_N_ELEMENTS (rock));
  g_autoptr (GVariant) preamp = get (f, "Preamp");
  g_assert_cmpfloat (g_variant_get_double (preamp), ==, -5);

  /* Y se guardó: el archivo existe y dice «rock». */
  g_autofree gchar *contents = NULL;
  g_assert_true (g_file_get_contents (f->path, &contents, NULL, NULL));
  g_assert_nonnull (strstr (contents, "preset=rock"));
  g_autoptr (GVariant) saved = get (f, "Saved");
  g_assert_true (g_variant_get_boolean (saved));
}

static void
test_set_gain_goes_custom_and_signals (Fixture *f,
                                       gconstpointer d G_GNUC_UNUSED)
{
  g_autoptr (GVariant) r1 = call (f, "SetPreset",
                                  g_variant_new ("(s)", "bass"), NULL);
  f->signals = 0;
  g_autoptr (GVariant) r2 = call (f, "SetGain",
                                  g_variant_new ("(ud)", 9, 3.0), NULL);
  g_assert_nonnull (r2);

  g_autoptr (GVariant) preset = get (f, "Preset");
  g_assert_cmpstr (g_variant_get_string (preset, NULL), ==, "custom");
  static const gdouble expected[] = { 6, 5, 4, 2, 0, 0, 0, 0, 0, 3 };
  g_autoptr (GVariant) custom = get (f, "CustomGains");
  assert_doubles (custom, expected, G_N_ELEMENTS (expected));

  /* Llegaron avisos —el del cambio y el de «guardado»— y el primero traía el
   * perfil y las ganancias nuevas. */
  wait_signals (f, 1);
  g_assert_cmpuint (f->signals, >=, 1);
  g_assert_nonnull (f->changed);
}

static void
test_signal_carries_what_changed (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  /* Con el guardado demorado, el aviso del cambio y el de «guardado» llegan
   * por separado, y se puede mirar el primero. */
  vasak_eq_service_free (f->service);
  f->service = vasak_eq_service_new (f->path, 60000, on_apply, f);
  g_assert_true (vasak_eq_service_export (f->service, f->server, NULL));

  f->signals = 0;
  g_autoptr (GVariant) r = call (f, "SetPreset",
                                 g_variant_new ("(s)", "treble"), NULL);
  wait_signals (f, 1);
  g_assert_cmpuint (f->signals, ==, 1);
  g_assert_true (changed_has (f, "Preset"));
  g_assert_true (changed_has (f, "Gains"));
  g_assert_true (changed_has (f, "Preamp"));
  /* Todavía no está en disco: la interfaz muestra «Sin guardar». */
  g_assert_true (changed_has (f, "Saved"));
  g_autoptr (GVariant) saved = get (f, "Saved");
  g_assert_false (g_variant_get_boolean (saved));
  /* No cambió ni el encendido ni el propio: no se avisan. */
  g_assert_false (changed_has (f, "Enabled"));
  g_assert_false (changed_has (f, "CustomGains"));

  vasak_eq_service_flush (f->service);
  wait_signals (f, 2);
  g_assert_cmpuint (f->signals, ==, 2);
  g_assert_true (changed_has (f, "Saved"));
  g_autoptr (GVariant) saved2 = get (f, "Saved");
  g_assert_true (g_variant_get_boolean (saved2));
}

static void
test_set_gains_and_enabled (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  gdouble gains[VASAK_EQ_BANDS] = { 1, 2, 3, 4, 5, -5, -4, -3, -2, -1 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) r = call (f, "SetGains",
      g_variant_new ("(@ad)", g_variant_new_fixed_array (
          G_VARIANT_TYPE_DOUBLE, gains, VASAK_EQ_BANDS, sizeof (gdouble))),
      &error);
  g_assert_no_error (error);
  g_autoptr (GVariant) got = get (f, "Gains");
  assert_doubles (got, gains, VASAK_EQ_BANDS);

  g_autoptr (GVariant) r2 = call (f, "SetEnabled",
                                  g_variant_new ("(b)", FALSE), &error);
  g_assert_no_error (error);
  g_assert_false (f->last_applied.enabled);
  g_autoptr (GVariant) enabled = get (f, "Enabled");
  g_assert_false (g_variant_get_boolean (enabled));

  /* Lo mismo dos veces no vuelve a aplicar: el slider que se suelta en el
   * mismo lugar no relanza nada. */
  guint before = f->applied;
  g_autoptr (GVariant) r3 = call (f, "SetEnabled",
                                  g_variant_new ("(b)", FALSE), NULL);
  g_assert_cmpuint (f->applied, ==, before);
}

static void
assert_invalid (Fixture *f, const gchar *method, GVariant *args)
{
  g_autoptr (GError) error = NULL;
  guint before = f->applied;
  g_autoptr (GVariant) r = call (f, method, args, &error);
  g_assert_null (r);
  g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_assert_cmpuint (f->applied, ==, before);
}

static void
test_invalid_arguments (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  assert_invalid (f, "SetPreset", g_variant_new ("(s)", "Rock"));
  assert_invalid (f, "SetPreset", g_variant_new ("(s)", ""));
  assert_invalid (f, "SetGain", g_variant_new ("(ud)", 10, 0.0));
  assert_invalid (f, "SetGain", g_variant_new ("(ud)", 0, 12.5));
  assert_invalid (f, "SetGain", g_variant_new ("(ud)", 0, NAN));

  gdouble three[3] = { 0, 0, 0 };
  assert_invalid (f, "SetGains", g_variant_new ("(@ad)",
      g_variant_new_fixed_array (G_VARIANT_TYPE_DOUBLE, three, 3,
                                 sizeof (gdouble))));

  /* Y lo que había sigue igual. */
  g_autoptr (GVariant) preset = get (f, "Preset");
  g_assert_cmpstr (g_variant_get_string (preset, NULL), ==, "flat");
}

static void
test_available (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  f->signals = 0;
  vasak_eq_service_set_available (f->service, TRUE);
  wait_signals (f, 1);
  g_assert_cmpuint (f->signals, ==, 1);
  g_assert_true (changed_has (f, "Available"));
  g_autoptr (GVariant) available = get (f, "Available");
  g_assert_true (g_variant_get_boolean (available));

  /* Decir lo mismo otra vez no avisa. */
  vasak_eq_service_set_available (f->service, TRUE);
  wait_signals (f, 2);
  g_assert_cmpuint (f->signals, ==, 1);
}

static void
test_restart_restores (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  g_autoptr (GVariant) r1 = call (f, "SetGain",
                                  g_variant_new ("(ud)", 4, -6.5), NULL);
  g_autoptr (GVariant) r2 = call (f, "SetEnabled",
                                  g_variant_new ("(b)", FALSE), NULL);

  /* Se va —como WirePlumber al cerrar la sesión— y vuelve. */
  vasak_eq_service_free (f->service);
  f->service = vasak_eq_service_new (f->path, 0, on_apply, f);
  const VasakEqState *s = vasak_eq_service_get_state (f->service);
  g_assert_cmpstr (s->preset, ==, "custom");
  g_assert_cmpfloat (s->custom[4], ==, -6.5);
  g_assert_false (s->enabled);
  g_assert_true (vasak_eq_service_export (f->service, f->server, NULL));
}

static void
test_unwritable_state_is_not_saved (Fixture *f, gconstpointer d G_GNUC_UNUSED)
{
  /* Un directorio en lugar del archivo: no se puede escribir, y `Saved` lo
   * tiene que decir en vez de mentir. */
  vasak_eq_service_free (f->service);
  g_assert_cmpint (g_mkdir (f->path, 0700), ==, 0);
  g_test_expect_message ("vasak-equalizer", G_LOG_LEVEL_WARNING,
                         "*no se pudo leer*");
  f->service = vasak_eq_service_new (f->path, 0, on_apply, f);
  g_test_assert_expected_messages ();
  g_assert_true (vasak_eq_service_export (f->service, f->server, NULL));

  g_test_expect_message ("vasak-equalizer", G_LOG_LEVEL_WARNING,
                         "*no se pudo guardar*");
  g_autoptr (GVariant) r = call (f, "SetPreset",
                                 g_variant_new ("(s)", "pop"), NULL);
  g_assert_nonnull (r);
  g_test_assert_expected_messages ();
  g_autoptr (GVariant) saved = get (f, "Saved");
  g_assert_false (g_variant_get_boolean (saved));
  /* Suena igual. */
  g_assert_cmpstr (f->last_applied.preset, ==, "pop");
  g_rmdir (f->path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
#define ADD(path, fn) \
  g_test_add (path, Fixture, NULL, setup, fn, teardown)
  ADD ("/service/frecuencias, rango y perfiles", test_read_only_description);
  ADD ("/service/arranca plano, encendido y guardado", test_initial_state);
  ADD ("/service/elegir un perfil suena, avisa y se guarda", test_set_preset);
  ADD ("/service/mover una banda pasa a custom",
       test_set_gain_goes_custom_and_signals);
  ADD ("/service/el aviso dice qué cambió, y después que se guardó",
       test_signal_carries_what_changed);
  ADD ("/service/diez de una vez, y apagar", test_set_gains_and_enabled);
  ADD ("/service/lo inválido se rechaza y no suena", test_invalid_arguments);
  ADD ("/service/Available sigue al nodo", test_available);
  ADD ("/service/después de reiniciar vuelve lo elegido",
       test_restart_restores);
  ADD ("/service/si no se puede guardar, Saved lo dice",
       test_unwritable_state_is_not_saved);
#undef ADD
  return g_test_run ();
}
