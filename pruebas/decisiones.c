/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * La tabla de decisiones: si un cliente tiene cámara, y si la entrada que
 * tenía le deja de sobrevivir.
 *
 * Es la línea más delicate del módulo y no la puede mirar nadie salvo esto: la
 * decisión estaba dentro de `modulo.c`, que es el único archivo del repositorio
 * que CI no compila, porque los encabezados de WirePlumber 0.5 no están en
 * Ubuntu. Ahora está en `decisiones.c`, que sólo incluye `<glib.h>`, y esta
 * prueba corre con glib pelado. Lo que se prueba acá es lo único que hay: una
 * tabla y las reglas de qué concede permiso.
 *
 * Dos reglas, y las dos importan en un solo sentido —equivocarse hacia negar
 * deja a alguien sin cámara y se nota, equivocarse hacia permitir no se nota
 * nunca—:
 *
 *  1. Lo único que concede la cámara es una decisión guardada que sea
 *     `PERMITIDA`. `NEGADA`, `SIN_DECIDIR` y que no haya nada dan cero.
 *  2. La entrada se borra cuando el cliente muere. Los ids de PipeWire se
 *     reciclan y un puntero liberado también se reutiliza, así que una entrada
 *     que sobrevive al cliente le entrega la cámara a quien caiga en esa
 *     dirección. Es el bug que CodeRabbit marcó en el PR #1, y el motivo de que
 *     la clave sea el objeto y no su id.
 *
 * Los clientes son punteros de mentira: la tabla nunca desreferencia la
 * clave, la compara por puntero, así que un número hace el mismo papel que un
 * `WpClient` y deja la prueba sin WirePlumber. Tampoco se mira la tabla por
 * dentro —ni el hash, ni su orden de recorrido, que GLib siembra por tabla—:
 * todo lo que se afirma acá sale de la interfaz.
 */

#include "decisiones.h"
#include <stdio.h>
/* Por el caso de la dirección reutilizada de más abajo, y sólo por ése: el
 * resto de la prueba es C11 y glib pelado. `MAP_ANONYMOUS` viene de
 * `asm-generic/mman-common.h`, que es el ABI del núcleo y no está detrás de
 * ningún `feature test macro`, así que `-std=c11` lo muestra sin ayuda. */
#include <sys/mman.h>

static int fallos = 0;

/* Los `por_omision` que se prueban.
 *
 * El de cero está primero y no es opcional: con un valor de cero, la rama que
 * concede y la que niega dan lo mismo, así que una prueba que sólo pasara cero
 * no probaría la regla. El último es lo que vale `PW_PERM_ALL` en PipeWire
 * (`~0u`), copiado a mano a propósito: incluir `pipewire/permission.h` para
 * leer una constante sería volver a atar esta prueba a un servidor que no está
 * mirando. */
#define POR_OMISION_NULO 0u
#define POR_OMISION_PARCIAL 0x0Fu    /* una máscara parcial, sin nombre */
#define POR_OMISION_TODO 0xFFFFFFFFu  /* lo que vale `PW_PERM_ALL` */
#define POR_OMISION_RARO 0x80000001u  /* un bit alto y uno bajo */

/* Direcciones inventadas, una por cliente de prueba. La clave se compara por
 * puntero y nunca se desreferencia, así que un número sirve de cliente. */
static gpointer
cliente (guint numero)
{
  return GSIZE_TO_POINTER (0x1000u + numero);
}

/* Los cuatro ayudantes de abajo —`igual_permiso`, `igual_decision`,
 * `igual_logico` y `estado`— terminan cada comprobación con un
 * `fflush (stdout)`, y no es prolijidad: `stdout` va a un archivo o a una
 * tubería, y ahí va acumulado. Si el código que se está probando revienta —una
 * fuga, un `g_error`, un segmento— lo que se pierde es justamente el buffer,
 * y la corrida termina en un código de salida sin una sola palabra de por
 * qué. Con el `fflush`, el rojo llega con la última comprobación que pasó
 * escrita. */
static void
igual_permiso (const char *caso, guint32 dio, guint32 esperado)
{
  printf ("  %s %s\n", dio == esperado ? "\033[32mok\033[0m"
                                       : "\033[31mMAL\033[0m", caso);
  if (dio != esperado) {
    printf ("      esperaba %" G_GUINT32_FORMAT " y dio %" G_GUINT32_FORMAT
            "\n", esperado, dio);
    fallos++;
  }
  fflush (stdout);
}

static void
igual_decision (const char *caso, VasakDecision dio, VasakDecision esperada)
{
  printf ("  %s %s\n", dio == esperada ? "\033[32mok\033[0m"
                                       : "\033[31mMAL\033[0m", caso);
  if (dio != esperada) {
    printf ("      esperaba %d y dio %d\n", esperada, dio);
    fallos++;
  }
  fflush (stdout);
}

static void
igual_logico (const char *caso, gboolean dio, gboolean esperado)
{
  printf ("  %s %s\n", dio == esperado ? "\033[32mok\033[0m"
                                       : "\033[31mMAL\033[0m", caso);
  if (dio != esperado) {
    printf ("      esperaba %s y dio %s\n", esperado ? "sí" : "no",
            dio ? "sí" : "no");
    fallos++;
  }
  fflush (stdout);
}

/**
 * El estado completo de un cliente, de los tres modos que se pueden mirar.
 *
 * Son tres hechos y van juntos porque juntos son el estado: lo que concede la
 * tabla (`…_permiso`), lo que tiene anotado (`…_consultar`) y si todavía tiene
 * una entrada para ese cliente. Un fallo en cualquiera de los tres tiene que
 * verse en la misma línea, porque son tres caras de lo mismo.
 *
 * El tercero se mide preguntando con `…_marcar_negada`, y **por eso va al
 * final**: preguntar si hay entrada agrega una si no la había. No cambia lo que
 * se puede observar —una entrada `NEGADA` y ninguna dan los mismos cero y
 * `NEGADA`—, así que el sondeo va después de las dos lecturas para no pisar lo
 * que se está mirando. Es el único modo de distinguir lo ausente de lo negado
 * por la interfaz, y por eso hace falta: `…_consultar` no los separa, y esa
 * indistinción es deliberada.
 *
 * Nada de esto toca la tabla por dentro. Una prueba que leyera el hash se
 * rompería en el próximo refactor sin haber encontrado ninguna regresión.
 */
static void
estado (const char *caso, VasakMediosDecisiones *d, gpointer c,
        guint32 por_omision, guint32 permiso_esperado,
        VasakDecision decision_esperada, gboolean ausente_esperado)
{
  guint32 permiso = vasak_medios_decisiones_permiso (d, c, por_omision);
  VasakDecision decision = vasak_medios_decisiones_consultar (d, c);
  gboolean ausente = vasak_medios_decisiones_marcar_negada (d, c);

  gboolean bien = (permiso == permiso_esperado) &&
                  (decision == decision_esperada) &&
                  (ausente == ausente_esperado);

  printf ("  %s %s\n", bien ? "\033[32mok\033[0m" : "\033[31mMAL\033[0m", caso);
  if (!bien) {
    printf ("      esperaba permiso=%" G_GUINT32_FORMAT " decision=%d "
            "sin entrada=%s\n", permiso_esperado, decision_esperada,
            ausente_esperado ? "sí" : "no");
    printf ("      dio     permiso=%" G_GUINT32_FORMAT " decision=%d "
            "sin entrada=%s\n", permiso, decision, ausente ? "sí" : "no");
    fallos++;
  }
  fflush (stdout);
}

/* ── 1. La regla de la línea ─────────────────────────────────────────────── */

/* La regla entera, con los tres estados y lo que no está, y cada estado
 * probado contra los tres `por_omision`.
 *
 * Cada comprobación usa un cliente distinto, y no por prolijidad: preguntar si
 * a un cliente le queda una entrada **se la agrega** si no la tenía, así que
 * repetir sobre el mismo cliente daría falso en la segunda vuelta y la prueba
 * estaría midiendo su propio sondeo. Un cliente por comprobación, y ninguna
 * necesita al anterior.
 *
 * Los tres `por_omision` porque el de cero esconde el error: con cero, conceder
 * y negar dan el mismo número, y una regla que concediera siempre pasaría. */
static void
regla_de_la_linea (void)
{
  const guint32 omisiones[] = { POR_OMISION_NULO, POR_OMISION_PARCIAL,
                                POR_OMISION_TODO };
  VasakMediosDecisiones *d = vasak_medios_decisiones_nueva ();
  guint c = 0;

  printf ("\nlo que no está en la tabla no da cámara\n");
  for (guint i = 0; i < G_N_ELEMENTS (omisiones); i++) {
    char caso[96];
    g_snprintf (caso, sizeof caso,
                "un cliente del que no sabemos nada, con por_omision %u",
                omisiones[i]);
    estado (caso, d, cliente (++c), omisiones[i], 0, VASAK_DECISION_NEGADA,
            TRUE);
  }

  printf ("\nlo que está negado tampoco\n");
  for (guint i = 0; i < G_N_ELEMENTS (omisiones); i++) {
    char caso[96];
    gpointer x = cliente (++c);
    g_snprintf (caso, sizeof caso, "anotado como negado, con por_omision %u",
                omisiones[i]);
    vasak_medios_decisiones_anotar (d, x, VASAK_DECISION_NEGADA);
    estado (caso, d, x, omisiones[i], 0, VASAK_DECISION_NEGADA, FALSE);
  }

  printf ("\ny sin decidir es negar con otro nombre, no es un «casi sí»\n");
  for (guint i = 0; i < G_N_ELEMENTS (omisiones); i++) {
    char caso[96];
    gpointer x = cliente (++c);
    g_snprintf (caso, sizeof caso, "sin decidir, con por_omision %u",
                omisiones[i]);
    vasak_medios_decisiones_anotar (d, x, VASAK_DECISION_SIN_DECIDIR);
    estado (caso, d, x, omisiones[i], 0, VASAK_DECISION_SIN_DECIDIR, FALSE);
  }

  printf ("\nlo único que concede es «permitida», y da lo que le pasaron\n");
  for (guint i = 0; i < G_N_ELEMENTS (omisiones); i++) {
    char caso[96];
    gpointer x = cliente (++c);
    g_snprintf (caso, sizeof caso, "permitida, con por_omision %u",
                omisiones[i]);
    vasak_medios_decisiones_anotar (d, x, VASAK_DECISION_PERMITIDA);
    /* Con `por_omision` cero lo esperado también es cero: conceder sin
     * `por_omision` no es conceder. La fila va igual, porque lo que comprueba
     * es que sea la rama de «permitida» la que devuelve. */
    estado (caso, d, x, omisiones[i], omisiones[i], VASAK_DECISION_PERMITIDA,
            FALSE);
  }

  /* Lo que se devuelve es lo que entró, no «los permisos que tocan»: una
   * máscara con un bit raro tiene que volver tal cual, porque normalizarla acá
   * sería inventarse permisos que el gestor no concedió. */
  gpointer raro = cliente (++c);
  vasak_medios_decisiones_anotar (d, raro, VASAK_DECISION_PERMITIDA);
  igual_permiso ("el por_omision vuelve tal cual, sin normalizar",
                 vasak_medios_decisiones_permiso (d, raro, POR_OMISION_RARO),
                 POR_OMISION_RARO);
  vasak_medios_decisiones_olvidar (d, raro);

  vasak_medios_decisiones_libre (d);
}

/* Cada tabla con lo suyo.
 *
 * El encabezado lo dice y no es un detalle: el módulo tiene una y las pruebas
 * hacen las que necesitan, y si dos compartieran entradas, un `limpiar` de una
 * vaciaría la otra y el «cliente» de una prueba empezaría con lo que la otra
 * dejó. Es la clase de defecto que no se ve en una prueba y se ve en dos. */
static void
cada_una_con_lo_suyo (void)
{
  VasakMediosDecisiones *una = vasak_medios_decisiones_nueva ();
  VasakMediosDecisiones *otra = vasak_medios_decisiones_nueva ();
  gpointer c = cliente (1);

  printf ("\ndos tablas no comparten entradas\n");
  vasak_medios_decisiones_anotar (una, c, VASAK_DECISION_PERMITIDA);
  igual_permiso ("lo anotado en una concede en esa",
                 vasak_medios_decisiones_permiso (una, c, POR_OMISION_TODO),
                 POR_OMISION_TODO);
  igual_permiso ("y en la otra no concede nada",
                 vasak_medios_decisiones_permiso (otra, c, POR_OMISION_TODO),
                 0);

  printf ("\ny vaciar una no vacía la otra\n");
  vasak_medios_decisiones_limpiar (una);
  igual_permiso ("la vaciada ya no concede",
                 vasak_medios_decisiones_permiso (una, c, POR_OMISION_TODO),
                 0);
  vasak_medios_decisiones_anotar (otra, c, VASAK_DECISION_PERMITIDA);
  igual_permiso ("y la otra sigue sirviendo",
                 vasak_medios_decisiones_permiso (otra, c, POR_OMISION_TODO),
                 POR_OMISION_TODO);

  vasak_medios_decisiones_libre (una);
  vasak_medios_decisiones_libre (otra);
}

/* ── 2. Una dirección liberada ───────────────────────────────────────────── */

/* Lo que sostiene todo lo demás, y lo que la entrada de `modulo.c` más vigila.
 *
 * La secuencia es la de verdad: un cliente pide la cámara, se la conceden, el
 * cliente muere y su entrada se va con él, y otro cliente —otro objeto, otra
 * vida— cae en la misma dirección. Si la entrada se quedara, el segundo entra
 * con la cámara sin haber preguntado: se la lleva con lo que decidió otro.
 */
static void
direccion_liberada (void)
{
  VasakMediosDecisiones *d = vasak_medios_decisiones_nueva ();
  gpointer c = cliente (1);

  printf ("\nconcedida, sin borrar la entrada, la dirección queda servida\n");
  vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_PERMITIDA);
  estado ("la entrada sobrevive mientras nadie la borre", d, c,
          POR_OMISION_TODO, POR_OMISION_TODO, VASAK_DECISION_PERMITIDA,
          FALSE);

  printf ("\npero al morir el cliente la entrada se va con él\n");
  vasak_medios_decisiones_olvidar (d, c);
  estado ("el mismo puntero, ya olvidado, no concede nada", d, c,
          POR_OMISION_TODO, 0, VASAK_DECISION_NEGADA, TRUE);

  printf ("\ny se borra la de los tres estados, no sólo la que se filtra\n");
  /* Si `olvidar` borrara sólo las `PERMITIDA`, la cámara no se filtraría por
   * ahí, pero la tabla seguiría guardando clientes muertos, y el enganche
   * débil dejaría de volver a engancharse para el siguiente ocupante. */
  {
    const VasakDecision estados[] = { VASAK_DECISION_NEGADA,
                                      VASAK_DECISION_PERMITIDA,
                                      VASAK_DECISION_SIN_DECIDIR };
    for (guint i = 0; i < G_N_ELEMENTS (estados); i++) {
      VasakMediosDecisiones *t = vasak_medios_decisiones_nueva ();
      gpointer x = cliente (10 + i);
      char caso[96];
      vasak_medios_decisiones_anotar (t, x, estados[i]);
      vasak_medios_decisiones_olvidar (t, x);
      g_snprintf (caso, sizeof caso, "olvidar saca la entrada del estado %d",
                  estados[i]);
      igual_logico (caso, vasak_medios_decisiones_marcar_negada (t, x), TRUE);
      vasak_medios_decisiones_libre (t);
    }
  }

  vasak_medios_decisiones_libre (d);
}

/* La misma historia con una dirección de verdad: memoria que se libera y
 * vuelve a repartirse.
 *
 * Los punteros de más arriba alcanzan para fijar la regla —la tabla nunca
 * desreferencia la clave—, pero la regla existe por una razón del mundo real:
 * que la memoria de un objeto liberado se vuelva a entregar. Eso se
 * reproduce acá de verdad, y se comprueba que cuando pasa la cámara no cambia
 * de dueño.
 *
 * Con `mmap` y no con `g_malloc` a propósito, y no por purismo. `malloc` lo
 * hace el asignador, y su política decide cuándo devuelve una dirección
 * liberada: con glibc pelado pasa siempre en la primera, pero un asignador con
 * cuarentena —el de un sanitizador, o el de un `MALLOC_PERTURB_`— la retiene y
 * el caso no se arma. Una prueba que depende de la política del asignador es
 * una prueba que puede no llegar a correr, y la que menos puede permitirse
 * eso es la que sostiene la razón de ser de la tabla. `mmap` no pasa por el
 * asignador: el núcleo devuelve la misma dirección liberada, siempre.
 *
 * El «objeto» se ubica con un desplazamiento dentro de la región para que la
 * dirección no sea la del principio de una página: un `WpClient` no está
 * alineado, y una dirección rare que no se parece a ninguna real esconde
 * justo la forma del error que se está probando. */
static gpointer
reservar_y_soltar (gsize largo)
{
  gpointer region = mmap (NULL, largo, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED)
    return NULL;
  munmap (region, largo);
  return region;
}

static void
direccion_liberada_de_verdad (void)
{
  VasakMediosDecisiones *d = vasak_medios_decisiones_nueva ();
  const gsize largo = 8192;
  const gsize desplazamiento = 512;
  gpointer antes = NULL;
  gpointer ahora = NULL;

  printf ("\ny con una dirección de verdad, liberada y repartida de nuevo\n");

  antes = (gpointer) ((guint8 *) reservar_y_soltar (largo) + desplazamiento);
  igual_logico ("hubo memoria que liberar y volver a repartir", antes != NULL,
                TRUE);
  if (antes == NULL) {
    vasak_medios_decisiones_libre (d);
    return;
  }

  /* El ocupante anterior de esa dirección tenía la cámara. */
  vasak_medios_decisiones_anotar (d, antes, VASAK_DECISION_PERMITIDA);
  igual_permiso ("el ocupante anterior de la dirección la tenía",
                 vasak_medios_decisiones_permiso (d, antes, POR_OMISION_TODO),
                 POR_OMISION_TODO);

  /* Muere, y con él su entrada. */
  vasak_medios_decisiones_olvidar (d, antes);

  /* Y la dirección vuelve a repartirse, ahora para otro. */
  ahora = (gpointer) ((guint8 *) reservar_y_soltar (largo) + desplazamiento);
  igual_logico ("la dirección libre se vuelve a repartir", ahora == antes,
                TRUE);
  if (ahora != antes) {
    vasak_medios_decisiones_libre (d);
    return;
  }

  estado ("el nuevo ocupante de la dirección no hereda la cámara", d, ahora,
          POR_OMISION_TODO, 0, VASAK_DECISION_NEGADA, TRUE);

  /* Y al ocupante nuevo se le concede lo suyo, y sólo lo suyo: la tabla sigue
   * sirviendo, no se quedó muerta por habernos llevamos la entrada. */
  vasak_medios_decisiones_anotar (d, ahora, VASAK_DECISION_PERMITIDA);
  igual_permiso ("y al ocupante nuevo se le puede conceder a él",
                 vasak_medios_decisiones_permiso (d, ahora, POR_OMISION_TODO),
                 POR_OMISION_TODO);

  vasak_medios_decisiones_olvidar (d, ahora);
  munmap ((guint8 *) ahora - desplazamiento, largo);
  vasak_medios_decisiones_libre (d);
}

/* ── 3. Marcar negada ────────────────────────────────────────────────────── */

/* Marca sólo la primera vez, y no pisa lo que haya.
 *
 * El enganche de `modulo.c` cuelga de este retorno: si mintiera, se
 * engancharía dos veces la referencia débil del mismo objeto. Y el
 * `TRUE`/`FALSE` no alcanza para probarlo: hay que mirar **el valor que
 * quedó**, porque una implementación que devuelve bien y pisa igual pasa la
 * mitad de la prueba. */
static void
marcar_negada (void)
{
  VasakMediosDecisiones *d = vasak_medios_decisiones_nueva ();
  gpointer c = cliente (1);

  printf ("\nla primera vez marca y avisa que hay que enganchar\n");
  gboolean primera = vasak_medios_decisiones_marcar_negada (d, c);
  igual_logico ("un cliente que no estaba en la tabla se marca", primera, TRUE);
  estado ("y queda anotado como negado", d, c, POR_OMISION_TODO, 0,
          VASAK_DECISION_NEGADA, FALSE);

  printf ("\nla segunda vez no: si mintiera, se cuelga dos veces\n");
  gboolean segunda = vasak_medios_decisiones_marcar_negada (d, c);
  igual_logico ("un cliente ya marcado no se vuelve a marcar", segunda, FALSE);
  estado ("y sigue estando negado", d, c, POR_OMISION_TODO, 0,
          VASAK_DECISION_NEGADA, FALSE);

  /* El caso que separa `contains` de un `lookup != NULL`. Un `NEGADA` guardado
   * es `GUINT_TO_POINTER (0)`, o sea `NULL`, así que un lookup no lo distingue
   * de «ausente»: con lookup, un cliente ya negado se volvería a marcar y a
   * enganchar su referencia débil por segunda vez. Sólo el `FALSE` lo ve; el
   * valor guardado es `NEGADA` en las dos implementaciones, y por eso lo que
   * separa a una de la otra es el retorno. */
  printf ("\ny un cliente ya negado tampoco se vuelve a marcar\n");
  {
    VasakMediosDecisiones *t = vasak_medios_decisiones_nueva ();
    gpointer x = cliente (2);
    vasak_medios_decisiones_anotar (t, x, VASAK_DECISION_NEGADA);
    gboolean dio = vasak_medios_decisiones_marcar_negada (t, x);
    igual_logico ("con un NEGADA ya anotado, no se vuelve a marcar", dio,
                  FALSE);
    estado ("y lo que queda anotado sigue siendo NEGADA, no otra cosa", t, x,
            POR_OMISION_TODO, 0, VASAK_DECISION_NEGADA, FALSE);
    vasak_medios_decisiones_libre (t);
  }

  printf ("\nni pisa un permiso concedido, que sería dejar de dar cámara\n");
  {
    VasakMediosDecisiones *t = vasak_medios_decisiones_nueva ();
    gpointer x = cliente (3);
    vasak_medios_decisiones_anotar (t, x, VASAK_DECISION_PERMITIDA);
    gboolean dio = vasak_medios_decisiones_marcar_negada (t, x);
    igual_logico ("con un PERMITIDA ya anotado, no se vuelve a marcar", dio,
                  FALSE);
    estado ("y el permiso sigue en pie", t, x, POR_OMISION_TODO,
            POR_OMISION_TODO, VASAK_DECISION_PERMITIDA, FALSE);
    vasak_medios_decisiones_libre (t);
  }

  printf ("\nni pisa un «sin decidir», que se registra aparte\n");
  {
    VasakMediosDecisiones *t = vasak_medios_decisiones_nueva ();
    gpointer x = cliente (4);
    vasak_medios_decisiones_anotar (t, x, VASAK_DECISION_SIN_DECIDIR);
    gboolean dio = vasak_medios_decisiones_marcar_negada (t, x);
    igual_logico ("con un SIN_DECIDIR ya anotado, no se vuelve a marcar", dio,
                  FALSE);
    estado ("y sigue sin decidir, no negado", t, x, POR_OMISION_TODO, 0,
            VASAK_DECISION_SIN_DECIDIR, FALSE);
    vasak_medios_decisiones_libre (t);
  }

  vasak_medios_decisiones_libre (d);
}

/* ── 4. Olvidar y limpiar ────────────────────────────────────────────────── */

/* Un cliente que se va no se lleva puesto a los demás, y vaciar la tabla la
 * deja **todavía servible**: es lo que hace `disable` al reencender, y una
 * tabla que no se puede volver a usar después de `limpiar` es un bug que
 * solamente aparece en el segundo encendido. */
static void
olvidar_y_limpiar (void)
{
  VasakMediosDecisiones *d = vasak_medios_decisiones_nueva ();
  gpointer a = cliente (1);
  gpointer b = cliente (2);
  gpointer c = cliente (3);

  printf ("\nolvidar a uno no toca a los otros\n");
  vasak_medios_decisiones_anotar (d, a, VASAK_DECISION_PERMITIDA);
  vasak_medios_decisiones_anotar (d, b, VASAK_DECISION_PERMITIDA);
  vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_PERMITIDA);

  vasak_medios_decisiones_olvidar (d, b);
  estado ("el olvidado queda sin cámara", d, b, POR_OMISION_TODO, 0,
          VASAK_DECISION_NEGADA, TRUE);
  igual_permiso ("y al que estaba antes no se lo tocaron",
                 vasak_medios_decisiones_permiso (d, a, POR_OMISION_TODO),
                 POR_OMISION_TODO);
  igual_permiso ("ni al que estaba después",
                 vasak_medios_decisiones_permiso (d, c, POR_OMISION_TODO),
                 POR_OMISION_TODO);

  printf ("\ny se puede olvidar de más, que es lo que pasa cuando el cliente\n"
          "  ya se había ido y el enganche dispara igual\n");
  vasak_medios_decisiones_olvidar (d, b);
  vasak_medios_decisiones_olvidar (d, cliente (99)); /* nunca estuvo */
  estado ("olvidar dos veces al mismo lo deja como estaba", d, b,
          POR_OMISION_TODO, 0, VASAK_DECISION_NEGADA, TRUE);
  igual_permiso ("y al que estaba antes tampoco",
                 vasak_medios_decisiones_permiso (d, a, POR_OMISION_TODO),
                 POR_OMISION_TODO);
  igual_permiso ("ni al que estaba después",
                 vasak_medios_decisiones_permiso (d, c, POR_OMISION_TODO),
                 POR_OMISION_TODO);

  printf ("\nlimpiar vacía la tabla entera\n");
  vasak_medios_decisiones_limpiar (d);
  estado ("el primero ya no está", d, a, POR_OMISION_TODO, 0,
          VASAK_DECISION_NEGADA, TRUE);
  estado ("ni el último", d, c, POR_OMISION_TODO, 0, VASAK_DECISION_NEGADA,
          TRUE);

  printf ("\ny una tabla vacía todavía sirve, que es lo que hace el\n"
          "  reencendido del módulo\n");
  /* Un cliente nuevo, no uno de los que ya se sondearon arriba: preguntar si le
   * queda entrada a un ausente se la crea. */
  gpointer nuevo = cliente (50);
  igual_logico ("se puede volver a marcar a un cliente nuevo",
                vasak_medios_decisiones_marcar_negada (d, nuevo), TRUE);
  vasak_medios_decisiones_anotar (d, nuevo, VASAK_DECISION_PERMITIDA);
  igual_permiso ("y se puede volver a conceder",
                 vasak_medios_decisiones_permiso (d, nuevo, POR_OMISION_TODO),
                 POR_OMISION_TODO);

  printf ("\nque se pueda vaciar y rellenar otra vez, y otra, sin que la\n"
          "  segunda vaciada sea distinta de la primera\n");
  for (int vuelta = 1; vuelta <= 2; vuelta++) {
    char caso[64];
    gpointer x = cliente (40 + (guint) vuelta);
    vasak_medios_decisiones_anotar (d, x, VASAK_DECISION_PERMITIDA);
    g_snprintf (caso, sizeof caso, "vuelta %d: concede antes de vaciar",
                vuelta);
    igual_permiso (caso,
                   vasak_medios_decisiones_permiso (d, x, POR_OMISION_TODO),
                   POR_OMISION_TODO);
    vasak_medios_decisiones_limpiar (d);
    g_snprintf (caso, sizeof caso, "vuelta %d: no concede después de vaciar",
                vuelta);
    igual_permiso (caso,
                   vasak_medios_decisiones_permiso (d, x, POR_OMISION_TODO), 0);
  }

  vasak_medios_decisiones_libre (d);
}

/* ── 5. Anotar pisa ──────────────────────────────────────────────────────── */

/* La última respuesta es la que vale. Es el camino del portal: entra un
 * `NEGADA` de arranque mientras la consulta viaja, y cuando la persona contesta
 * esa respuesta pisa el valor de partida. Pisar en las dos direcciones importa
 * por igual: una respuesta que llega `SIN_DECIDIR` sobre un permiso que ya se
 * había concedido tiene que quitar la cámara, no quedarse a medio camino. */
static void
anotar_pisa (void)
{
  VasakMediosDecisiones *d = vasak_medios_decisiones_nueva ();

  printf ("\nel camino del portal: el NEGADA de arranque pisa a permitido\n");
  {
    gpointer c = cliente (1);
    vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_NEGADA);
    igual_permiso ("de salida no hay cámara",
                   vasak_medios_decisiones_permiso (d, c, POR_OMISION_TODO), 0);
    vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_PERMITIDA);
    estado ("y cuando la persona concede, concede", d, c, POR_OMISION_TODO,
            POR_OMISION_TODO, VASAK_DECISION_PERMITIDA, FALSE);
  }

  printf ("\ny al revés también: una respuesta posterior puede quitar\n"
          "  la cámara que ya estaba\n");
  {
    gpointer c = cliente (2);
    vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_PERMITIDA);
    vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_SIN_DECIDIR);
    estado ("de permitida a sin decidir", d, c, POR_OMISION_TODO, 0,
            VASAK_DECISION_SIN_DECIDIR, FALSE);

    vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_PERMITIDA);
    vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_NEGADA);
    estado ("de permitida a negada", d, c, POR_OMISION_TODO, 0,
            VASAK_DECISION_NEGADA, FALSE);

    vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_SIN_DECIDIR);
    vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_PERMITIDA);
    estado ("de sin decidir a permitida", d, c, POR_OMISION_TODO,
            POR_OMISION_TODO, VASAK_DECISION_PERMITIDA, FALSE);
  }

  printf ("\nuna respuesta que llega después de olvidado también concede:\n"
          "  es un cliente nuevo que ya cumplió con su consulta\n");
  {
    gpointer c = cliente (3);
    vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_PERMITIDA);
    vasak_medios_decisiones_olvidar (d, c);
    vasak_medios_decisiones_anotar (d, c, VASAK_DECISION_PERMITIDA);
    estado ("olvidado y de vuelta con permiso", d, c, POR_OMISION_TODO,
            POR_OMISION_TODO, VASAK_DECISION_PERMITIDA, FALSE);
  }

  printf ("\ny pisar a uno no pisa a los demás\n");
  {
    gpointer a = cliente (4);
    gpointer b = cliente (5);
    vasak_medios_decisiones_anotar (d, a, VASAK_DECISION_PERMITIDA);
    vasak_medios_decisiones_anotar (d, b, VASAK_DECISION_PERMITIDA);
    vasak_medios_decisiones_anotar (d, a, VASAK_DECISION_NEGADA);
    igual_permiso ("el que se reescribió pierde la cámara",
                   vasak_medios_decisiones_permiso (d, a, POR_OMISION_TODO), 0);
    igual_permiso ("y el otro la sigue teniendo",
                   vasak_medios_decisiones_permiso (d, b, POR_OMISION_TODO),
                   POR_OMISION_TODO);
  }

  vasak_medios_decisiones_libre (d);
}

/* ── 6. Ante una tabla que no existe ─────────────────────────────────────── */

/* Falla hacia negar, también cuando lo que falla es el propio módulo. Un `NULL`
 * acá es un error de programación, no un caso de ejecución: lo que importa es
 * que la respuesta sea cero y no un permiso, porque el otro lado del módulo es
 * el que concede la cámara.
 *
 * Los `g_return_val_if_fail` avisan con un `critical` y el aviso está bien —no
 * es ruido que se pueda tapar en el módulo—, así que acá se traga la salida del
 * registro y lo que se afirma es lo que devuelven las funciones. El escritor
 * **no se vuelve a poner**: `g_log_set_writer_func` no acepta
 * `g_log_writer_default` como función para restaurar, y lo que queda de la
 * prueba después de esto no tiene nada que decir con el registro. */
static GLogWriterOutput
tragarse (GLogLevelFlags nivel G_GNUC_UNUSED,
          const GLogField *campos G_GNUC_UNUSED, gsize n G_GNUC_UNUSED,
          gpointer datos G_GNUC_UNUSED)
{
  return G_LOG_WRITER_HANDLED;
}

static void
sin_tabla (void)
{
  printf ("\nuna tabla que no existe no concede nada\n");
  guint32 por_omision = POR_OMISION_TODO;

  g_log_set_writer_func (tragarse, NULL, NULL);
  igual_permiso ("sin tabla, el permiso es cero",
                 vasak_medios_decisiones_permiso (NULL, cliente (1),
                                                  por_omision),
                 0);
  igual_decision ("sin tabla, la decisión es negar",
                  vasak_medios_decisiones_consultar (NULL, cliente (1)),
                  VASAK_DECISION_NEGADA);

  printf ("\ny liberar una tabla que no hay tampoco es un problema\n");
  vasak_medios_decisiones_libre (NULL);
  igual_logico ("liberar NULL no rompe nada", TRUE, TRUE);
}

/* Un cliente que ya estaba y al que le llega una respuesta, con la respuesta
 * que sea, nunca queda con la cámara puesta. Es la versión del enumerable
 * anterior, pero corrida contra `anotar`, que es por donde entran de verdad las
 * respuestas del servicio. */
static void
respuesta_desconocida (void)
{
  VasakMediosDecisiones *d = vasak_medios_decisiones_nueva ();
  gpointer c = cliente (1);

  printf ("\nuna respuesta que este módulo no conoce no da cámara\n");
  /* El enum tiene tres valores y el servicio sólo dice «allowed» o «denied»,
   * así que un valor futuro tiene que negar igual. Se anota el número tal
   * cual, que es lo que haría un `switch` sin `default` bien puesto. */
  vasak_medios_decisiones_anotar (d, c, (VasakDecision) 7);
  igual_permiso ("un estado que no existe no concede",
                 vasak_medios_decisiones_permiso (d, c, POR_OMISION_TODO), 0);

  vasak_medios_decisiones_libre (d);
}

int
main (void)
{
  regla_de_la_linea ();
  cada_una_con_lo_suyo ();
  direccion_liberada ();
  direccion_liberada_de_verdad ();
  marcar_negada ();
  olvidar_y_limpiar ();
  anotar_pisa ();
  sin_tabla ();
  respuesta_desconocida ();

  printf ("\n%s\n", fallos == 0 ? "\033[32mSin fallos.\033[0m"
                                : "\033[31mHay fallos.\033[0m");
  return fallos == 0 ? 0 : 1;
}
