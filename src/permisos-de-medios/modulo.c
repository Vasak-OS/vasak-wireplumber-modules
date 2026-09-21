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
  /* Consultas que llegaron antes que el bus del sistema. */
  GPtrArray *pendientes;
  GCancellable *cancelable;
};

G_DECLARE_FINAL_TYPE (VasakPermisosMedios, vasak_permisos_medios,
                      VASAK, PERMISOS_MEDIOS, WpPlugin)

typedef struct _Consulta Consulta;
static void consulta_libre (Consulta *c);
G_DEFINE_TYPE (VasakPermisosMedios, vasak_permisos_medios, WP_TYPE_PLUGIN)

static void
vasak_permisos_medios_init (VasakPermisosMedios *self)
{
  self->decisiones = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->pendientes = g_ptr_array_new_with_free_func ((GDestroyNotify) consulta_libre);
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

  /*
   * La identidad es **el objeto cliente**, no su id.
   *
   * Los ids globales de PipeWire se reciclan. Con la tabla indexada por id, y
   * sin limpiarla cuando el cliente se va, un cliente posterior que recibiera
   * el mismo número heredaba el «permitida» de otro y se llevaba la cámara.
   * Es la misma trampa que el servicio de permisos evita fijando el pid, un
   * piso más arriba, y acá estaba reintroducida — la marcó CodeRabbit en el
   * PR #1.
   *
   * Indexar por el puntero del objeto lo cierra sólo si la entrada se borra
   * cuando el objeto muere, porque un puntero liberado también se reutiliza.
   * De eso se ocupa `al_morir_el_cliente()`, enganchado con
   * `g_object_weak_ref` en el mismo momento en que se adjunta el gestor.
   */
  gpointer guardada = g_hash_table_lookup (self->decisiones, cliente);

  /* Lo que no está en la tabla vale cero, que es NEGADA. Eso cubre los tres
   * casos que importan: el cliente cuya consulta todavía no volvió, aquel
   * cuya consulta falló, y el que nunca llegó a preguntarse. Mientras no
   * sepamos que sí, es que no. */
  return (GPOINTER_TO_UINT (guardada) == VASAK_DECISION_PERMITIDA)
             ? por_omision
             : 0;
}

/* Lo que hace falta recordar mientras la consulta va y viene.
 *
 * El cliente va como referencia **débil**: si se desconecta mientras la
 * consulta viaja, la respuesta no tiene a quién aplicarse y se descarta. Una
 * referencia fuerte lo mantendría vivo de más y una cruda podría apuntar a
 * otro. */
struct _Consulta
{
  VasakPermisosMedios *self; /* con referencia: la consulta lo sobrevive */
  GWeakRef cliente;
  gchar *nombre;
  pid_t pid;
  guint64 inicio;
};

static void
consulta_libre (Consulta *c)
{
  g_weak_ref_clear (&c->cliente);
  g_clear_object (&c->self);
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
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      /* No se anota nada: la tabla ya dice NEGADA, y dejarlo así es lo
       * correcto. Si el servicio no contesta, no hay permiso que mostrar. */
      wp_warning ("no se pudo consultar el permiso de cámara de '%s': %s — "
                  "queda sin cámara",
                  c->nombre, error->message);
    consulta_libre (c);
    return;
  }

  /* ¿Sigue siendo el mismo cliente? Si se fue, la respuesta se tira: anotarla
   * dejaría un permiso suelto esperando a que alguien herede su lugar. */
  g_autoptr (WpClient) cliente = g_weak_ref_get (&c->cliente);
  if (cliente == NULL) {
    wp_debug ("'%s' se desconectó antes de la respuesta; se descarta",
              c->nombre);
    consulta_libre (c);
    return;
  }

  const gchar *texto = NULL;
  g_variant_get (respuesta, "(&s)", &texto);
  VasakDecision decision = vasak_medios_decision_desde_texto (texto);

  g_hash_table_insert (c->self->decisiones, cliente,
                       GUINT_TO_POINTER (decision));

  wp_info ("cámara para '%s': %s", c->nombre,
           decision == VASAK_DECISION_PERMITIDA     ? "permitida"
           : decision == VASAK_DECISION_SIN_DECIDIR ? "sin decidir, o sea que no"
                                                    : "negada");

  /* Recalcular. Sin esto la respuesta queda guardada y no llega al cliente:
   * los permisos se empujan acá, no al leerlos. */
  wp_permission_manager_update_permissions (c->self->gestor);
  consulta_libre (c);
}

static void
enviar_consulta (Consulta *c)
{
  g_dbus_connection_call (
      c->self->bus, VASAK_SERVICIO, VASAK_RUTA, VASAK_INTERFAZ, VASAK_METODO,
      g_variant_new ("(uts)", (guint32) c->pid, c->inicio,
                     VASAK_RECURSO_CAMARA),
      G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, c->self->cancelable,
      al_contestar_el_servicio, c);
}

static void
preguntar_por (VasakPermisosMedios *self, WpClient *cliente,
               const gchar *nombre, pid_t pid, guint64 inicio)
{
  Consulta *c = g_new0 (Consulta, 1);
  c->self = g_object_ref (self);
  g_weak_ref_init (&c->cliente, cliente);
  c->nombre = g_strdup (nombre ? nombre : "?");
  c->pid = pid;
  c->inicio = inicio;

  /*
   * El bus del sistema se pide de forma asíncrona al activarse el módulo, así
   * que los primeros clientes de la sesión pueden llegar antes que él. Quedan
   * encolados y se preguntan cuando el bus está.
   *
   * Descartarlos sería fallar cerrado, que suena bien y no lo es: son justo
   * los clientes que arrancan con la sesión, y quedarían **sin cámara para
   * siempre** aunque la persona se la haya concedido, sin nada que lo vuelva
   * a intentar. Mientras tanto siguen negados, que es lo correcto.
   */
  if (self->bus == NULL) {
    wp_debug ("todavía no hay bus del sistema: '%s' queda en la cola",
              c->nombre);
    g_ptr_array_add (self->pendientes, c);
    return;
  }

  enviar_consulta (c);
}

/* Cuando el cliente se va, su decisión se va con él. Sin esto la tabla crece
 * sola y —peor— deja un permiso esperando a que otro objeto ocupe la misma
 * dirección. */
static void
al_morir_el_cliente (gpointer datos, GObject *muerto)
{
  VasakPermisosMedios *self = datos;
  g_hash_table_remove (self->decisiones, muerto);
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

  /* Entra en la tabla ya negado, y se engancha su muerte para que la entrada
   * no le sobreviva. Las dos cosas acá y no al contestar: entre que se
   * pregunta y que llega la respuesta el cliente puede irse. */
  if (!g_hash_table_contains (self->decisiones, cliente)) {
    g_hash_table_insert (self->decisiones, cliente,
                         GUINT_TO_POINTER (VASAK_DECISION_NEGADA));
    g_object_weak_ref (G_OBJECT (cliente), al_morir_el_cliente, self);
  }

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
    /* Y la cola se tira: sin bus no hay a quién preguntarle, y los clientes
     * encolados quedan negados, que es la dirección correcta. No se reintenta
     * tomar el bus; si eso hiciera falta se vería como «nadie tiene cámara»,
     * que es visible y no silencioso. */
    g_ptr_array_set_size (self->pendientes, 0);
    return;
  }

  /* La cola: los clientes que se conectaron antes que el bus. Sin esto quedan
   * negados para siempre aunque la persona les haya dado permiso, porque nada
   * los vuelve a mirar. */
  if (self->pendientes->len > 0) {
    wp_info_object (self, "bus del sistema listo: se preguntan %u consultas "
                          "que estaban esperando",
                    self->pendientes->len);

    /* Se sacan del arreglo sin liberarlas: `enviar_consulta` toma la
     * propiedad y la libera al contestar. */
    g_autoptr (GPtrArray) cola = g_ptr_array_new ();
    for (guint i = 0; i < self->pendientes->len; i++)
      g_ptr_array_add (cola, g_ptr_array_index (self->pendientes, i));
    g_ptr_array_set_free_func (self->pendientes, NULL);
    g_ptr_array_set_size (self->pendientes, 0);
    g_ptr_array_set_free_func (self->pendientes,
                               (GDestroyNotify) consulta_libre);

    for (guint i = 0; i < cola->len; i++)
      enviar_consulta (g_ptr_array_index (cola, i));
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
  g_ptr_array_set_size (self->pendientes, 0);
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
