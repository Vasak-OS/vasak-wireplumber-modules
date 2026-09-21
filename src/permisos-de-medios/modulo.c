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
#include "servicio.h"

#include <wp/wp.h>
#include <pipewire/permission.h>

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
  WpPermissionManager *gestor;
  GDBusConnection *bus;
  /* id del cliente -> VasakDecision. Lo que no está es cero, que es NEGADA:
   * un cliente del que todavía no sabemos nada no tiene la cámara. */
  GHashTable *decisiones;
  GCancellable *cancelable;
};

G_DECLARE_FINAL_TYPE (VasakPermisosMedios, vasak_permisos_medios,
                      VASAK, PERMISOS_MEDIOS, WpPlugin)
G_DEFINE_TYPE (VasakPermisosMedios, vasak_permisos_medios, WP_TYPE_PLUGIN)

static void
vasak_permisos_medios_init (VasakPermisosMedios *self)
{
  self->decisiones = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->cancelable = g_cancellable_new ();
}

/*
 * Qué permisos tiene un cliente sobre un objeto.
 *
 * El interés con que se registra esto es amplio a propósito —cualquier objeto
 * con `media.class`— y quien decide es `vasak_medios_es_camara()`, que está
 * probada. Acotar el interés a la cámara dejaría la regla escrita en dos
 * lugares, y el día que uno cambie el otro no: negar de más deja al escritorio
 * sin compartir pantalla, negar de menos deja la cámara abierta, y las dos se
 * ven igual de bien mirando el código.
 */
static guint32
permisos_sobre (WpPermissionManager *gestor, WpClient *cliente,
                WpGlobalProxy *objeto, gpointer datos)
{
  VasakPermisosMedios *self = datos;

  g_autoptr (WpProperties) props =
      wp_pipewire_object_get_properties (WP_PIPEWIRE_OBJECT (objeto));
  guint32 por_omision = wp_permission_manager_get_default_permissions (gestor);

  if (props == NULL)
    return por_omision;

  if (!vasak_medios_es_camara (wp_properties_get (props, "media.class"),
                               wp_properties_get (props, "media.role"),
                               wp_properties_get (props, "device.api")))
    return por_omision;

  guint id = wp_proxy_get_bound_id (WP_PROXY (cliente));
  gpointer guardada = g_hash_table_lookup (self->decisiones,
                                           GUINT_TO_POINTER (id));

  /* Lo que no está en la tabla vale cero, que es NEGADA. Eso cubre los dos
   * casos que importan: el cliente cuya consulta todavía no volvió, y aquel
   * cuya consulta falló. Mientras no sepamos que sí, es que no. */
  return (GPOINTER_TO_UINT (guardada) == VASAK_DECISION_PERMITIDA)
             ? por_omision
             : 0;
}

/* Lo que hace falta recordar mientras la consulta va y viene. */
typedef struct
{
  VasakPermisosMedios *self;
  guint id_cliente;
  gchar *nombre;
} Consulta;

static void
consulta_libre (Consulta *c)
{
  g_free (c->nombre);
  g_free (c);
}

static void
al_contestar_el_servicio (GObject *fuente, GAsyncResult *res, gpointer datos)
{
  Consulta *c = datos;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) respuesta = g_dbus_connection_call_finish (
      G_DBUS_CONNECTION (fuente), res, &error);

  if (respuesta == NULL) {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      consulta_libre (c);
      return;
    }
    /* No se anota nada: la tabla ya dice NEGADA por omisión, y dejarlo así es
     * lo correcto. Si el servicio no contesta, no hay permiso que mostrar. */
    wp_warning ("no se pudo consultar el permiso de cámara de '%s': %s — "
                "queda sin cámara",
                c->nombre, error->message);
    consulta_libre (c);
    return;
  }

  const gchar *texto = NULL;
  g_variant_get (respuesta, "(&s)", &texto);
  VasakDecision decision = vasak_medios_decision_desde_texto (texto);

  g_hash_table_insert (c->self->decisiones,
                       GUINT_TO_POINTER (c->id_cliente),
                       GUINT_TO_POINTER (decision));

  wp_info ("cámara para '%s': %s", c->nombre,
           decision == VASAK_DECISION_PERMITIDA   ? "permitida"
           : decision == VASAK_DECISION_SIN_DECIDIR ? "sin decidir, o sea que no"
                                                    : "negada");

  /* Recalcular. Sin esto la respuesta queda guardada y no llega al cliente:
   * los permisos se empujan acá, no al leerlos. */
  wp_permission_manager_update_permissions (c->self->gestor);
  consulta_libre (c);
}

static void
preguntar_por (VasakPermisosMedios *self, WpClient *cliente,
               const gchar *nombre, pid_t pid, guint64 inicio)
{
  if (self->bus == NULL) {
    wp_warning ("sin bus del sistema: '%s' queda sin cámara", nombre);
    return;
  }

  Consulta *c = g_new0 (Consulta, 1);
  c->self = self;
  c->id_cliente = wp_proxy_get_bound_id (WP_PROXY (cliente));
  c->nombre = g_strdup (nombre ? nombre : "?");

  g_dbus_connection_call (
      self->bus, VASAK_SERVICIO, VASAK_RUTA, VASAK_INTERFAZ, VASAK_METODO,
      g_variant_new ("(uts)", (guint32) pid, inicio, VASAK_RECURSO_CAMARA),
      G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, self->cancelable,
      al_contestar_el_servicio, c);
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
al_seleccionar_acceso (WpEvent *evento, gpointer datos)
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

  VasakPermisosMedios *self = datos;

  /* Si otro ya decidió por este cliente —la configuración, o el portal— lo
   * suyo vale. El del portal es el camino que pregunta, y pisarlo dejaría a
   * una aplicación con la cámara concedida sin poder usarla. */
  if (wp_event_get_data (evento, "permission-manager") != NULL ||
      wp_event_get_data (evento, "default-permissions") != NULL) {
    wp_debug_object (cliente, "'%s' ya tiene quien decida por él",
                     nombre ? nombre : "?");
    return;
  }

  /* El gestor se adjunta **antes** de preguntar, y por eso el cliente queda
   * sin cámara desde el primer momento: la tabla todavía no dice nada sobre
   * él, y lo que no está vale NEGADA. La respuesta sólo puede mejorar eso.
   *
   * Al revés —preguntar primero y adjuntar después— dejaría una ventana con
   * la cámara abierta, que es exactamente la carrera que ya tiene WirePlumber
   * y que no hay por qué agrandar. */
  GValue valor = G_VALUE_INIT;
  g_value_init (&valor, G_TYPE_OBJECT);
  g_value_set_object (&valor, self->gestor);
  wp_event_set_data (evento, "permission-manager", &valor);
  g_value_unset (&valor);

  preguntar_por (self, cliente, nombre, pid, inicio);
}

static void
al_tener_el_bus (GObject *fuente G_GNUC_UNUSED, GAsyncResult *res,
                 gpointer datos)
{
  g_autoptr (VasakPermisosMedios) self = datos;
  g_autoptr (GError) error = NULL;

  self->bus = g_bus_get_finish (res, &error);
  if (self->bus == NULL) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      wp_warning_object (self,
                         "sin bus del sistema (%s): nadie va a tener cámara",
                         error->message);
    return;
  }
}

static void
vasak_permisos_medios_enable (WpPlugin *plugin,
                              WpTransition *transition G_GNUC_UNUSED)
{
  VasakPermisosMedios *self = VASAK_PERMISOS_MEDIOS (plugin);
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));

  /*
   * Después del de configuración y **antes del que reparte lo de por
   * omisión**, que es exactamente donde se pone `find-portal-access`.
   *
   * Nombrar sólo `apply-access` no alcanza y el fallo es silencioso: los dos
   * quedan «antes de apply-access» sin orden entre sí,
   * `client/find-default-access` gana la carrera, pone su gestor, y este
   * enganche se encuentra con que alguien ya decidió y se aparta. Medido —el
   * registro decía «ya tiene quien decida por él» para todos los clientes— y
   * se ve como que el módulo no niega nada.
   */
  static const gchar *antes[] = { "client/find-default-access",
                                  "client/apply-access", NULL };
  static const gchar *despues[] = { "client/find-config-access", NULL };

  /*
   * El gestor por omisión da **todos** los permisos: lo único que se quita es
   * lo que `permisos_sobre()` reconozca como cámara. Un gestor que negara por
   * omisión le sacaría al cliente el audio y todo lo demás.
   */
  self->gestor = wp_permission_manager_new (core);
  wp_permission_manager_set_default_permissions (self->gestor, PW_PERM_ALL);
  wp_permission_manager_add_interest_match (
      self->gestor, permisos_sobre, self,
      wp_object_interest_new (WP_TYPE_GLOBAL_PROXY,
                              WP_CONSTRAINT_TYPE_PW_PROPERTY, "media.class",
                              "+", NULL, NULL));

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

  /*
   * El bus del sistema, que es donde vive `vasak-permissions` — corre como
   * root porque tiene que leer `/proc/<pid>/exe` de procesos ajenos.
   *
   * Se pide de forma asíncrona y el enganche ya quedó registrado: si un
   * cliente se conecta antes de que el bus esté, `preguntar_por()` no tiene
   * por dónde preguntar y lo deja sin cámara. Es la dirección correcta de
   * fallar, y es preferible a esperar al bus con el enganche sin registrar,
   * que dejaría a esos clientes sin gestor y **con** la cámara.
   */
  g_bus_get (G_BUS_TYPE_SYSTEM, self->cancelable, al_tener_el_bus,
             g_object_ref (self));

  wp_info_object (self, "permisos-de-medios activo");
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

  g_cancellable_cancel (self->cancelable);
  g_clear_object (&self->cancelable);
  self->cancelable = g_cancellable_new ();

  g_clear_object (&self->bus);
  g_clear_object (&self->gestor);
  g_hash_table_remove_all (self->decisiones);

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
