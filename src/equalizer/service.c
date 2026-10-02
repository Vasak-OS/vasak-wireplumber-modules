/* SPDX-License-Identifier: GPL-3.0-or-later */

#define G_LOG_DOMAIN "vasak-equalizer"

#include <string.h>

#include "service.h"

/* La interfaz, escrita una vez. El README la copia con explicaciones; la
 * prueba `tests/equalizer/service.c` comprueba que lo que dice el README se
 * pueda llamar. */
static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='" VASAK_EQ_INTERFACE "'>"
  "    <method name='SetGain'>"
  "      <arg type='u' name='band' direction='in'/>"
  "      <arg type='d' name='gain' direction='in'/>"
  "    </method>"
  "    <method name='SetGains'>"
  "      <arg type='ad' name='gains' direction='in'/>"
  "    </method>"
  "    <method name='SetPreset'>"
  "      <arg type='s' name='preset' direction='in'/>"
  "    </method>"
  "    <method name='SetEnabled'>"
  "      <arg type='b' name='enabled' direction='in'/>"
  "    </method>"
  "    <property name='Frequencies' type='ad' access='read'/>"
  "    <property name='GainRange' type='(dd)' access='read'/>"
  "    <property name='Presets' type='as' access='read'/>"
  "    <property name='Preset' type='s' access='read'/>"
  "    <property name='Gains' type='ad' access='read'/>"
  "    <property name='CustomGains' type='ad' access='read'/>"
  "    <property name='Preamp' type='d' access='read'/>"
  "    <property name='Enabled' type='b' access='read'/>"
  "    <property name='Available' type='b' access='read'/>"
  "    <property name='Saved' type='b' access='read'/>"
  "  </interface>"
  "</node>";

struct _VasakEqService {
  gchar *state_path;
  guint save_delay_ms;
  VasakEqApplyFunc apply;
  gpointer user_data;

  VasakEqState state;
  /* Lo último que se escribió bien. `Saved` es que coincida con `state`. */
  VasakEqState on_disk;
  gboolean on_disk_known;
  gboolean available;
  guint save_source;

  GDBusNodeInfo *info;
  GDBusConnection *connection;
  guint registration;
};

/* Lo que se ve desde afuera, para saber qué cambió. */
typedef struct {
  VasakEqState state;
  gboolean available;
  gboolean saved;
} Snapshot;

static gboolean
is_saved (const VasakEqService *self)
{
  return self->on_disk_known &&
         vasak_eq_state_equal (&self->state, &self->on_disk);
}

static Snapshot
snapshot (VasakEqService *self)
{
  return (Snapshot) { self->state, self->available, is_saved (self) };
}

static GVariant *
doubles (const gdouble *values, gsize n)
{
  return g_variant_new_fixed_array (G_VARIANT_TYPE_DOUBLE, values, n,
                                    sizeof (gdouble));
}

static GVariant *
property_value (VasakEqService *self, const gchar *name)
{
  gdouble gains[VASAK_EQ_BANDS];
  vasak_eq_state_gains (&self->state, gains);

  if (g_str_equal (name, "Frequencies")) {
    gdouble f[VASAK_EQ_BANDS];
    for (guint i = 0; i < VASAK_EQ_BANDS; i++)
      f[i] = vasak_eq_band_frequency (i);
    return doubles (f, VASAK_EQ_BANDS);
  }
  if (g_str_equal (name, "GainRange"))
    return g_variant_new ("(dd)", VASAK_EQ_GAIN_MIN, VASAK_EQ_GAIN_MAX);
  if (g_str_equal (name, "Presets")) {
    guint n = 0;
    const VasakEqPreset *presets = vasak_eq_presets (&n);
    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE_STRING_ARRAY);
    for (guint i = 0; i < n; i++)
      g_variant_builder_add (&b, "s", presets[i].id);
    return g_variant_builder_end (&b);
  }
  if (g_str_equal (name, "Preset"))
    return g_variant_new_string (self->state.preset);
  if (g_str_equal (name, "Gains"))
    return doubles (gains, VASAK_EQ_BANDS);
  if (g_str_equal (name, "CustomGains"))
    return doubles (self->state.custom, VASAK_EQ_BANDS);
  if (g_str_equal (name, "Preamp"))
    return g_variant_new_double (vasak_eq_preamp (gains));
  if (g_str_equal (name, "Enabled"))
    return g_variant_new_boolean (self->state.enabled);
  if (g_str_equal (name, "Available"))
    return g_variant_new_boolean (self->available);
  /* "Saved": la última de la introspección. */
  return g_variant_new_boolean (is_saved (self));
}

/* Avisa por `PropertiesChanged` lo que difiera entre `before` y ahora. */
static void
notify (VasakEqService *self, const Snapshot *before)
{
  if (self->connection == NULL || self->registration == 0)
    return;

  Snapshot now = snapshot (self);
  gdouble old_gains[VASAK_EQ_BANDS];
  gdouble new_gains[VASAK_EQ_BANDS];
  vasak_eq_state_gains (&before->state, old_gains);
  vasak_eq_state_gains (&now.state, new_gains);
  gboolean gains_changed = memcmp (old_gains, new_gains, sizeof old_gains) != 0;

  const gchar *changed[8];
  guint n = 0;
  if (g_strcmp0 (before->state.preset, now.state.preset) != 0)
    changed[n++] = "Preset";
  if (gains_changed) {
    changed[n++] = "Gains";
    if (vasak_eq_preamp (old_gains) != vasak_eq_preamp (new_gains))
      changed[n++] = "Preamp";
  }
  if (memcmp (before->state.custom, now.state.custom,
              sizeof now.state.custom) != 0)
    changed[n++] = "CustomGains";
  if (before->state.enabled != now.state.enabled)
    changed[n++] = "Enabled";
  if (before->available != now.available)
    changed[n++] = "Available";
  if (before->saved != now.saved)
    changed[n++] = "Saved";
  if (n == 0)
    return;

  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE_VARDICT);
  for (guint i = 0; i < n; i++)
    g_variant_builder_add (&b, "{sv}", changed[i],
                           property_value (self, changed[i]));

  g_dbus_connection_emit_signal (
      self->connection, NULL, VASAK_EQ_OBJECT_PATH,
      "org.freedesktop.DBus.Properties", "PropertiesChanged",
      g_variant_new ("(sa{sv}as)", VASAK_EQ_INTERFACE, &b, NULL), NULL);
}

static void
write_now (VasakEqService *self)
{
  g_autoptr (GError) error = NULL;
  if (vasak_eq_state_save (&self->state, self->state_path, &error)) {
    self->on_disk = self->state;
    self->on_disk_known = TRUE;
  } else {
    /* `Saved` se queda en falso, que es lo que la interfaz muestra como «Sin
     * guardar». Suena igual; lo que no va a hacer es sobrevivir al reinicio, y
     * eso es lo que se avisa. */
    g_warning ("no se pudo guardar el ecualizador en %s: %s",
               self->state_path, error->message);
  }
}

static gboolean
save_timeout (gpointer data)
{
  VasakEqService *self = data;
  Snapshot before = snapshot (self);
  self->save_source = 0;
  write_now (self);
  notify (self, &before);
  return G_SOURCE_REMOVE;
}

static void
schedule_save (VasakEqService *self)
{
  if (self->save_source != 0)
    g_source_remove (self->save_source);
  self->save_source = g_timeout_add (self->save_delay_ms, save_timeout, self);
}

/* Después de cada cambio aceptado: que suene, que se avise, que se guarde. */
static void
changed (VasakEqService *self, const Snapshot *before)
{
  if (vasak_eq_state_equal (&before->state, &self->state))
    return;
  if (self->apply != NULL)
    self->apply (&self->state, self->user_data);
  schedule_save (self);
  notify (self, before);
}

static void
invalid_args (GDBusMethodInvocation *invocation, const gchar *message)
{
  g_dbus_method_invocation_return_dbus_error (
      invocation, "org.freedesktop.DBus.Error.InvalidArgs", message);
}

static void
method_call (GDBusConnection *connection G_GNUC_UNUSED,
             const gchar *sender G_GNUC_UNUSED,
             const gchar *object_path G_GNUC_UNUSED,
             const gchar *interface_name G_GNUC_UNUSED,
             const gchar *method_name, GVariant *parameters,
             GDBusMethodInvocation *invocation, gpointer data)
{
  VasakEqService *self = data;
  Snapshot before = snapshot (self);

  if (g_str_equal (method_name, "SetGain")) {
    guint32 band;
    gdouble gain;
    g_variant_get (parameters, "(ud)", &band, &gain);
    if (!vasak_eq_state_set_gain (&self->state, band, gain)) {
      invalid_args (invocation, "banda fuera de 0..9 o ganancia fuera de "
                                "GainRange");
      return;
    }
  } else if (g_str_equal (method_name, "SetGains")) {
    g_autoptr (GVariant) array = g_variant_get_child_value (parameters, 0);
    gsize n = 0;
    const gdouble *gains = g_variant_get_fixed_array (array, &n,
                                                      sizeof (gdouble));
    if (n != VASAK_EQ_BANDS ||
        !vasak_eq_state_set_gains (&self->state, gains)) {
      invalid_args (invocation, "hacen falta diez ganancias dentro de "
                                "GainRange");
      return;
    }
  } else if (g_str_equal (method_name, "SetPreset")) {
    const gchar *id;
    g_variant_get (parameters, "(&s)", &id);
    if (!vasak_eq_state_set_preset (&self->state, id)) {
      invalid_args (invocation, "perfil desconocido: ver Presets, o «custom»");
      return;
    }
  } else {
    /* `SetEnabled`. GDBus ya rechazó lo que no está en la introspección —una
     * llamada desconocida o con otra firma no llega hasta acá—, así que lo
     * único que queda es este. */
    gboolean enabled;
    g_variant_get (parameters, "(b)", &enabled);
    self->state.enabled = enabled;
  }

  changed (self, &before);
  g_dbus_method_invocation_return_value (invocation, NULL);
}

/* GDBus contesta solo por las propiedades que no están en la introspección:
 * acá llegan únicamente las diez de arriba. */
static GVariant *
get_property (GDBusConnection *connection G_GNUC_UNUSED,
              const gchar *sender G_GNUC_UNUSED,
              const gchar *object_path G_GNUC_UNUSED,
              const gchar *interface_name G_GNUC_UNUSED,
              const gchar *property_name, GError **error G_GNUC_UNUSED,
              gpointer data)
{
  return property_value (data, property_name);
}

static const GDBusInterfaceVTable vtable = {
  .method_call = method_call,
  .get_property = get_property,
};

VasakEqService *
vasak_eq_service_new (const gchar *state_path, guint save_delay_ms,
                      VasakEqApplyFunc apply, gpointer user_data)
{
  VasakEqService *self = g_new0 (VasakEqService, 1);
  self->state_path = g_strdup (state_path);
  self->save_delay_ms = save_delay_ms;
  self->apply = apply;
  self->user_data = user_data;
  self->info = g_dbus_node_info_new_for_xml (introspection_xml, NULL);

  g_autoptr (GError) error = NULL;
  if (!vasak_eq_state_load (&self->state, state_path, &error)) {
    g_warning ("no se pudo leer %s (%s): se arranca con lo de fábrica",
               state_path, error->message);
    /* Lo que hay en disco no es lo que suena: `Saved` en falso hasta que se
     * escriba, y no se pisa el archivo hasta que la persona cambie algo —
     * puede que se arregle a mano. */
    self->on_disk_known = FALSE;
  } else {
    self->on_disk = self->state;
    /* Si no existía, «guardado» es verdad igual: lo de fábrica es lo que
     * vuelve a sonar después de un reinicio. */
    self->on_disk_known = TRUE;
  }
  return self;
}

void
vasak_eq_service_flush (VasakEqService *self)
{
  if (self->save_source == 0)
    return;
  g_source_remove (self->save_source);
  save_timeout (self);
}

void
vasak_eq_service_free (VasakEqService *self)
{
  if (self == NULL)
    return;
  vasak_eq_service_flush (self);
  vasak_eq_service_unexport (self);
  g_clear_pointer (&self->info, g_dbus_node_info_unref);
  g_free (self->state_path);
  g_free (self);
}

gboolean
vasak_eq_service_export (VasakEqService *self, GDBusConnection *connection,
                         GError **error)
{
  g_return_val_if_fail (self->registration == 0, FALSE);
  self->registration = g_dbus_connection_register_object (
      connection, VASAK_EQ_OBJECT_PATH, self->info->interfaces[0], &vtable,
      self, NULL, error);
  if (self->registration == 0)
    return FALSE;
  self->connection = g_object_ref (connection);
  return TRUE;
}

void
vasak_eq_service_unexport (VasakEqService *self)
{
  if (self->registration != 0)
    g_dbus_connection_unregister_object (self->connection,
                                         self->registration);
  self->registration = 0;
  g_clear_object (&self->connection);
}

void
vasak_eq_service_set_available (VasakEqService *self, gboolean available)
{
  Snapshot before = snapshot (self);
  self->available = available;
  notify (self, &before);
}

const VasakEqState *
vasak_eq_service_get_state (const VasakEqService *self)
{
  return &self->state;
}
