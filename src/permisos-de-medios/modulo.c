/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * permisos-de-medios — identifica a cada cliente de PipeWire por su pid.
 *
 * ── Qué hace hoy ────────────────────────────────────────────────────────────
 *
 * **Todavía no niega nada.** Se engancha donde se deciden los accesos, resuelve
 * quién es cada cliente y anota lo que preguntaría. Eso es a propósito y es la
 * primera de dos etapas: antes de quitarle la cámara a alguien conviene ver
 * qué clientes aparecen en una sesión de verdad.
 *
 * ── Por qué existe ──────────────────────────────────────────────────────────
 *
 * `50-vasak-camara.conf` en `vasak-desktop-settings` ya no le ofrece la cámara
 * a quien entra por `pipewire-0`, y el que la quiera la pide por el portal.
 * Eso deja tres agujeros, y los tres son el mismo: no hay forma, **con
 * configuración sola**, de saber quién es el cliente.
 *
 *   · `pipewire.sec.socket` dice de forma confiable por dónde entró alguien,
 *     pero no le impide elegir por dónde entrar: los tres sockets son
 *     `srw-rw-rw-` y basta `PIPEWIRE_REMOTE=pipewire-0-priv`.
 *   · `application.process.binary` lo declara el propio cliente.
 *   · `pipewire.sec.label` —el perfil de AppArmor— lo pone el kernel, pero
 *     cualquier proceso sin confinar se pone el que quiera con `aa-exec`.
 *     Medido: un binario renombrado reclamando el perfil de Chrome capturó
 *     5 de 5.
 *
 * Queda `pipewire.sec.pid`, que lo fija el servidor desde las credenciales de
 * la conexión: no se puede falsear **ni elegir**. Es el único.
 *
 * ── Por qué en C y no en Lua ────────────────────────────────────────────────
 *
 * El Lua de WirePlumber no tiene `io`: no puede leer `/proc` en absoluto, y
 * hace falta para el instante de arranque del proceso.
 *
 * ── Qué NO hace, ni va a hacer ──────────────────────────────────────────────
 *
 * No resuelve `/proc/<pid>/exe`. Eso lo hace `vasak-permissions`, como root y
 * fijando el pid contra la reutilización, que es exactamente lo que ya hace
 * para identificar a quien lo llama. Acá sólo se juntan los dos números que
 * ese servicio necesita y se le pasan. Duplicar la resolución sería una copia
 * que se va a separar de la original.
 */

#include "camara.h"
#include "identidad.h"

#include <wp/wp.h>

WP_DEFINE_LOCAL_LOG_TOPIC ("m-vasak-medios")

/*
 * El socket por el que entra WirePlumber. Los clientes que vengan de ahí se
 * saltean, y no es una comodidad: WirePlumber es uno de ellos, y restringirse
 * a sí mismo le saca los dispositivos y voltea la pila de audio entera.
 * Probado, sin querer, mientras se medía la etapa 2.
 */
#define SOCKET_DEL_GESTOR "pipewire-0-manager"

struct _VasakPermisosMedios
{
  WpPlugin parent;
  WpEventHook *enganche;
};

G_DECLARE_FINAL_TYPE (VasakPermisosMedios, vasak_permisos_medios,
                      VASAK, PERMISOS_MEDIOS, WpPlugin)
G_DEFINE_TYPE (VasakPermisosMedios, vasak_permisos_medios, WP_TYPE_PLUGIN)

static void
vasak_permisos_medios_init (VasakPermisosMedios *self G_GNUC_UNUSED)
{
}

/*
 * Corre en cada `select-access`, o sea una vez por cliente que se conecta.
 *
 * El closure recibe **un solo** parametro, el evento, mas el `user_data` que
 * agrega `g_cclosure_new`. El encabezado de WirePlumber dice otra cosa --«the
 * closure should accept two parameters: the event dispatcher and the event»--
 * y es falso en 0.5.17: `wp_simple_event_hook_run` invoca con aridad **uno**.
 *
 * Creerle al encabezado no da error de compilacion ni caida: el segundo
 * argumento pasa a ser el `user_data`, `wp_event_get_subject()` sobre eso no
 * devuelve un cliente, y el enganche **se calla**. Parece que no dispara nunca.
 */
static void
al_seleccionar_acceso (WpEvent *evento, gpointer datos G_GNUC_UNUSED)
{
  g_autoptr (WpClient) cliente = WP_CLIENT (wp_event_get_subject (evento));
  if (cliente == NULL)
    return;

  g_autoptr (WpProperties) props = wp_pipewire_object_get_properties (
      WP_PIPEWIRE_OBJECT (cliente));
  if (props == NULL)
    return;

  const gchar *socket = wp_properties_get (props, "pipewire.sec.socket");
  const gchar *nombre = wp_properties_get (props, "application.name");

  if (g_strcmp0 (socket, SOCKET_DEL_GESTOR) == 0) {
    wp_debug_object (cliente, "se saltea '%s': entra por %s",
                     nombre ? nombre : "?", SOCKET_DEL_GESTOR);
    return;
  }

  const gchar *pid_txt = wp_properties_get (props, "pipewire.sec.pid");
  if (pid_txt == NULL) {
    /* Sin pid no hay a quién preguntarle. Pasa con los clientes internos del
     * propio daemon, que no vienen de un socket. */
    wp_debug_object (cliente, "se saltea '%s': no trae pipewire.sec.pid",
                     nombre ? nombre : "?");
    return;
  }

  pid_t pid = (pid_t) g_ascii_strtoll (pid_txt, NULL, 10);
  guint64 inicio = 0;

  if (!vasak_medios_inicio_del_proceso (pid, &inicio)) {
    /* El proceso se fue entre que conectó y que llegamos acá. Cuando esto
     * niegue de verdad, esto tiene que ser una negación y no un permiso. */
    wp_info_object (cliente,
                    "no se pudo fijar la identidad de '%s' (pid %d): el "
                    "proceso ya no está",
                    nombre ? nombre : "?", (int) pid);
    return;
  }

  /*
   * Acá va, en la segunda etapa, la consulta a `vasak-permissions`:
   *
   *     CheckPermissionFor(pid, inicio, "device.camera", "")
   *
   * sobre `ar.net.vasak.os.Permissions` en el bus del sistema, y con la
   * respuesta se le adjunta al cliente un gestor de permisos que oculte los
   * objetos que `vasak_medios_es_camara()` reconozca.
   *
   * La consulta es asíncrona y la respuesta tarda: mientras no llegue, el
   * cliente tiene que quedar **sin** la cámara y no con ella. Fallar abriendo
   * acá es no tener permiso.
   */
  wp_info_object (cliente,
                  "identificado '%s': pid %d, arranque %" G_GUINT64_FORMAT
                  " — todavía no se le niega nada",
                  nombre ? nombre : "?", (int) pid, inicio);
}

static void
vasak_permisos_medios_enable (WpPlugin *plugin,
                              WpTransition *transition G_GNUC_UNUSED)
{
  VasakPermisosMedios *self = VASAK_PERMISOS_MEDIOS (plugin);
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));

  /*
   * Después de los que reparten acceso y antes del que lo aplica, que es donde
   * el de configuración y el del portal se ponen. Si alguno de ésos ya decidió,
   * lo suyo vale: acá sólo se mira.
   */
  static const gchar *antes[] = { "client/apply-access", NULL };
  static const gchar *despues[] = { "client/find-config-access", NULL };

  self->enganche = wp_simple_event_hook_new (
      "vasak/permisos-de-medios", antes, despues,
      g_cclosure_new (G_CALLBACK (al_seleccionar_acceso), self, NULL));

  wp_interest_event_hook_add_interest (
      WP_INTEREST_EVENT_HOOK (self->enganche),
      WP_CONSTRAINT_TYPE_PW_PROPERTY, "event.type", "=s", "select-access",
      NULL);

  g_autoptr (WpEventDispatcher) despachador =
      wp_event_dispatcher_get_instance (core);
  wp_event_dispatcher_register_hook (despachador, self->enganche);

  wp_info_object (self, "permisos-de-medios activo (sólo anota, no niega)");
  wp_object_update_features (WP_OBJECT (self), WP_PLUGIN_FEATURE_ENABLED, 0);
}

static void
vasak_permisos_medios_disable (WpPlugin *plugin)
{
  VasakPermisosMedios *self = VASAK_PERMISOS_MEDIOS (plugin);
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));

  if (self->enganche != NULL) {
    g_autoptr (WpEventDispatcher) despachador =
        wp_event_dispatcher_get_instance (core);
    wp_event_dispatcher_unregister_hook (despachador, self->enganche);
    g_clear_object (&self->enganche);
  }

  wp_object_update_features (WP_OBJECT (self), 0, WP_PLUGIN_FEATURE_ENABLED);
}

static void
vasak_permisos_medios_class_init (VasakPermisosMediosClass *klass)
{
  WpPluginClass *plugin_class = (WpPluginClass *) klass;
  plugin_class->enable = vasak_permisos_medios_enable;
  plugin_class->disable = vasak_permisos_medios_disable;
}

WP_PLUGIN_EXPORT GObject *
wireplumber__module_init (WpCore *core, WpSpaJson *args G_GNUC_UNUSED,
                          GError **error G_GNUC_UNUSED)
{
  return G_OBJECT (g_object_new (vasak_permisos_medios_get_type (),
                                 "name", "vasak-permisos-de-medios",
                                 "core", core,
                                 NULL));
}
