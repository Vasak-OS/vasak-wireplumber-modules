/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * El módulo de WirePlumber del ecualizador.
 *
 * Hace tres cosas, y ninguna decide nada —lo que decide está en `state.c` y
 * `service.c`, que se prueban sin WirePlumber—:
 *
 * 1. **Encuentra el filtro**: el nodo `vasak-equalizer` que carga PipeWire
 *    desde `60-vasak-equalizer.conf`. Cada vez que aparece —al arrancar, o
 *    porque PipeWire se reinició— le pone las ganancias guardadas. Sin esto
 *    el filtro arranca plano y se queda plano.
 * 2. **Le pone las ganancias** con un `Props` de `params`, que es como
 *    `filter-chain` recibe sus controles: `"eq_31:Gain" 6.0 …`. Es el mismo
 *    camino que usa `node/filter-graph.lua` de WirePlumber.
 * 3. **Lo apaga y lo prende** con `filter.smart.disabled` en el metadato
 *    `filters`, que es lo que la política de filtros de WirePlumber mira para
 *    desengancharlo. Apagado, los flujos van directo al dispositivo.
 *
 * ── Lo que no hace ──────────────────────────────────────────────────────────
 *
 * **No sigue a la salida.** Eso lo hace WirePlumber solo, porque el filtro se
 * declara `filter.smart`: ver el fragmento. Escribir acá un enganche que
 * reenlazara al cambiar de salida sería competir con la política de enlaces
 * por los mismos enlaces, y en WirePlumber los enganches mal ordenados pierden
 * en silencio (lo sabemos por el otro módulo de este repositorio). Por eso
 * este módulo **no registra ningún enganche de eventos**.
 *
 * **No toca permisos.** El nodo es del propio demonio de PipeWire, no de un
 * cliente, así que el gestor de permisos de `permisos-de-medios` no lo ve
 * pasar; y lo que este módulo escribe —un parámetro del nodo y una clave del
 * metadato `filters`— lo escribe WirePlumber, que entra por el socket del
 * gestor. No se abre ningún camino nuevo hacia nada.
 */

#include <wp/wp.h>

#include "presets.h"
#include "service.h"
#include "state.h"

WP_DEFINE_LOCAL_LOG_TOPIC ("m-vasak-equalizer")

/* El metadato que mira la política de filtros de WirePlumber. Lo crea
 * `metadata.lua`, con este nombre, en la configuración de fábrica. */
#define FILTERS_METADATA "filters"

/* Medio segundo después del último cambio. Ver `vasak_eq_service_new()`. */
#define SAVE_DELAY_MS 500

struct _VasakEqualizer
{
  WpPlugin parent;

  VasakEqService *service;
  WpObjectManager *om;
  WpNode *node;
  WpMetadata *metadata;
  guint bus_owner;
};

G_DECLARE_FINAL_TYPE (VasakEqualizer, vasak_equalizer, VASAK, EQUALIZER,
                      WpPlugin)
G_DEFINE_TYPE (VasakEqualizer, vasak_equalizer, WP_TYPE_PLUGIN)

static void
vasak_equalizer_init (VasakEqualizer *self G_GNUC_UNUSED)
{
}

static void
apply_gains (VasakEqualizer *self, const VasakEqState *state)
{
  gdouble gains[VASAK_EQ_BANDS];
  vasak_eq_state_gains (state, gains);

  g_autoptr (WpSpaPodBuilder) list = wp_spa_pod_builder_new_struct ();
  wp_spa_pod_builder_add_string (list, VASAK_EQ_PREAMP_NODE ":Gain");
  wp_spa_pod_builder_add_float (list, (float) vasak_eq_preamp (gains));
  for (guint i = 0; i < VASAK_EQ_BANDS; i++) {
    g_autofree gchar *control =
        g_strdup_printf ("%s:Gain", vasak_eq_band_node (i));
    wp_spa_pod_builder_add_string (list, control);
    wp_spa_pod_builder_add_float (list, (float) gains[i]);
  }
  g_autoptr (WpSpaPod) params = wp_spa_pod_builder_end (list);

  g_autoptr (WpSpaPodBuilder) object =
      wp_spa_pod_builder_new_object ("Spa:Pod:Object:Param:Props", "Props");
  wp_spa_pod_builder_add_property (object, "params");
  wp_spa_pod_builder_add_pod (object, params);

  /* `set_param` se queda con el pod. */
  if (!wp_pipewire_object_set_param (WP_PIPEWIRE_OBJECT (self->node), "Props",
                                     0, wp_spa_pod_builder_end (object)))
    wp_warning_object (self, "el filtro no aceptó las ganancias");
}

static void
apply_enabled (VasakEqualizer *self, const VasakEqState *state)
{
  guint32 id = wp_proxy_get_bound_id (WP_PROXY (self->node));
  /* Encendido se escribe borrando la clave, no poniendo «false»: así el
   * metadato queda como lo deja WirePlumber, y lo que diga el nodo —que no
   * dice nada— vuelve a mandar. */
  wp_metadata_set (self->metadata, id, "filter.smart.disabled",
                   state->enabled ? NULL : "Spa:String:JSON",
                   state->enabled ? NULL : "true");
}

static void
apply (const VasakEqState *state, gpointer data)
{
  VasakEqualizer *self = data;
  if (self->node == NULL)
    return;
  apply_gains (self, state);
  if (self->metadata != NULL)
    apply_enabled (self, state);
  wp_debug_object (self, "aplicado: %s, perfil «%s»",
                   state->enabled ? "encendido" : "apagado", state->preset);
}

static void
on_object_added (WpObjectManager *om G_GNUC_UNUSED, GObject *object,
                 gpointer data)
{
  VasakEqualizer *self = data;

  if (WP_IS_NODE (object)) {
    g_set_object (&self->node, WP_NODE (object));
    wp_info_object (self, "filtro encontrado (id %u)",
                    wp_proxy_get_bound_id (WP_PROXY (object)));
    vasak_eq_service_set_available (self->service, TRUE);
  } else if (WP_IS_METADATA (object)) {
    g_set_object (&self->metadata, WP_METADATA (object));
  } else {
    return;
  }
  /* Con cualquiera de los dos que llegue se aplica todo: no hay orden
   * garantizado entre el nodo y el metadato, y `apply` hace lo que puede con
   * lo que haya. */
  apply (vasak_eq_service_get_state (self->service), self);
}

static void
on_object_removed (WpObjectManager *om G_GNUC_UNUSED, GObject *object,
                   gpointer data)
{
  VasakEqualizer *self = data;

  if ((GObject *) self->node == object) {
    g_clear_object (&self->node);
    wp_info_object (self, "el filtro se fue");
    vasak_eq_service_set_available (self->service, FALSE);
  } else if ((GObject *) self->metadata == object) {
    g_clear_object (&self->metadata);
  }
}

static void
on_bus_acquired (GDBusConnection *connection, const gchar *name G_GNUC_UNUSED,
                 gpointer data)
{
  VasakEqualizer *self = data;
  g_autoptr (GError) error = NULL;
  if (!vasak_eq_service_export (self->service, connection, &error))
    wp_warning_object (self, "no se pudo publicar %s: %s",
                       VASAK_EQ_OBJECT_PATH, error->message);
}

static void
on_name_lost (GDBusConnection *connection, const gchar *name,
              gpointer data)
{
  VasakEqualizer *self = data;
  /* Sin conexión es que no hay bus de la sesión —WirePlumber arrancado a mano,
   * fuera de una sesión—, y con conexión es que otro proceso ya tiene el
   * nombre. Las dos cosas dejan el filtro sonando con lo guardado, sólo que
   * sin poder cambiarlo. */
  wp_warning_object (self, "sin el nombre %s en el bus de la sesión (%s): "
                           "el ecualizador suena pero no se puede cambiar",
                     name, connection == NULL ? "no hay bus" : "lo tiene otro");
}

static void
vasak_equalizer_enable (WpPlugin *plugin,
                        WpTransition *transition G_GNUC_UNUSED)
{
  VasakEqualizer *self = VASAK_EQUALIZER (plugin);
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));

  g_autofree gchar *path = vasak_eq_state_default_path ();
  self->service = vasak_eq_service_new (path, SAVE_DELAY_MS, apply, self);

  self->om = wp_object_manager_new ();
  wp_object_manager_add_interest (self->om, WP_TYPE_NODE,
      WP_CONSTRAINT_TYPE_PW_PROPERTY, "node.name", "=s", VASAK_EQ_FILTER_NODE,
      NULL);
  wp_object_manager_add_interest (self->om, WP_TYPE_METADATA,
      WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY, "metadata.name", "=s",
      FILTERS_METADATA, NULL);
  wp_object_manager_request_object_features (self->om, WP_TYPE_GLOBAL_PROXY,
                                             WP_OBJECT_FEATURES_ALL);
  g_signal_connect_object (self->om, "object-added",
                           G_CALLBACK (on_object_added), self, 0);
  g_signal_connect_object (self->om, "object-removed",
                           G_CALLBACK (on_object_removed), self, 0);
  wp_core_install_object_manager (core, self->om);

  self->bus_owner = g_bus_own_name (G_BUS_TYPE_SESSION, VASAK_EQ_BUS_NAME,
                                    G_BUS_NAME_OWNER_FLAGS_NONE,
                                    on_bus_acquired, NULL, on_name_lost,
                                    self, NULL);

  wp_info_object (self, "ecualizador activo, estado en %s", path);
  wp_object_update_features (WP_OBJECT (self), WP_PLUGIN_FEATURE_ENABLED, 0);
}

static void
vasak_equalizer_disable (WpPlugin *plugin)
{
  VasakEqualizer *self = VASAK_EQUALIZER (plugin);

  if (self->bus_owner != 0)
    g_bus_unown_name (self->bus_owner);
  self->bus_owner = 0;
  /* Si se va apagado, que no deje al filtro desenganchado para siempre: sin
   * el módulo nadie lo volvería a enganchar hasta reiniciar PipeWire. */
  if (self->node != NULL && self->metadata != NULL)
    wp_metadata_set (self->metadata,
                     wp_proxy_get_bound_id (WP_PROXY (self->node)),
                     "filter.smart.disabled", NULL, NULL);
  g_clear_object (&self->om);
  g_clear_object (&self->node);
  g_clear_object (&self->metadata);
  /* Escribe lo pendiente antes de irse: un cambio hecho medio segundo antes de
   * cerrar la sesión también se guarda. */
  g_clear_pointer (&self->service, vasak_eq_service_free);

  wp_object_update_features (WP_OBJECT (self), 0, WP_PLUGIN_FEATURE_ENABLED);
}

static void
vasak_equalizer_class_init (VasakEqualizerClass *klass)
{
  WpPluginClass *plugin_class = (WpPluginClass *) klass;
  plugin_class->enable = vasak_equalizer_enable;
  plugin_class->disable = vasak_equalizer_disable;
}

WP_PLUGIN_EXPORT GObject *
wireplumber__module_init (WpCore *core, WpSpaJson *args G_GNUC_UNUSED,
                          GError **error G_GNUC_UNUSED)
{
  return G_OBJECT (g_object_new (vasak_equalizer_get_type (),
                                 "name", "vasak-equalizer",
                                 "core", core,
                                 NULL));
}
