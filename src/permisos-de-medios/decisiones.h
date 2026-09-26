/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * La tabla de decisiones: qué cliente tiene la cámara, y cuál todavía no se
 * sabe.
 *
 * ── Por qué está aparte ─────────────────────────────────────────────────────
 *
 * Esto es lo único de este módulo que *decide*, y hasta ahora vivía dentro de
 * `modulo.c`, que es el único archivo del repositorio que no se puede compilar
 * en CI: incluye `<wp/wp.h>` y `<pipewire/permission.h>`, y los encabezados de
 * WirePlumber 0.5 no están en los repositorios de Ubuntu. La decisión —«este
 * cliente sí, este otro todavía no lo sabemos»— era entonces código que nadie
 * podía ejecutar, y es justo la línea donde equivocarse entrega la cámara sin
 * que nada se entere.
 *
 * Acá no hay nada de WirePlumber: sólo `<glib.h>` y una tabla. Por eso se
 * compila y se prueba en un runner pelado, como los otros tres.
 *
 * ── La identidad es el puntero del objeto, no el id ─────────────────────────
 *
 * Los ids globales de PipeWire se reciclan. Con la tabla indexada por id, y
 * sin limpiarla cuando el cliente se va, un cliente posterior que recibiera el
 * mismo número heredaba el «permitida» de otro y se llevaba la cámara. Es la
 * misma trampa que el servicio de permisos evita fijando el pid, un piso más
 * arriba, y acá estaba reintroducida — la marcó CodeRabbit en el PR #1.
 *
 * Indexar por el puntero del objeto lo cierra sólo si la entrada se borra
 * cuando el objeto muere, porque un puntero liberado también se reutiliza. De
 * eso se ocupa el que llama, con `g_object_weak_ref`: `modulo.c` engancha esa
 * referencia en `al_morir_el_cliente()` y llama a `…_olvidar()`.
 *
 * O sea: la clave es un `gpointer` pelado y **es** el objeto cliente. Nadie
 * tiene que traducirlo, y por lo mismo `modulo.c` le pasa el `WpClient` tal
 * cual. Cambiar la clave por el id —que parece más prolijo y no lo es—
 * reintroduce el bug que terminó en el PR #1.
 */
#pragma once

#include <glib.h>

/* `VasakDecision` es de acá, no de esta unidad: son tres estados y están
 * definidos una vez, en el contrato con el servicio. */
#include "servicio.h"

/**
 * Una tabla de decisiones, con su estado propio.
 *
 * Cada instancia tiene la suya, y dos no comparten entradas: el módulo tiene
 * una y las pruebas hacen las que necesitan. `…_nueva()` devuelve una tabla
 * vacía y `…_libre()` la destruye.
 */
typedef struct _VasakMediosDecisiones VasakMediosDecisiones;

/** Una tabla nueva, vacía. */
VasakMediosDecisiones *vasak_medios_decisiones_nueva (void);

/**
 * Libera la tabla y todas sus entradas. Acepta `NULL`.
 *
 * El módulo **no** la llama, y no es un olvido: `VasakPermisosMedios` no tiene
 * `finalize`, y darle uno sería cambiarle la vida al plugin en un cambio que no
 * cambia nada. La tabla se crea una vez, en `_init`, y en `disable` se vacía
 * con `…_limpiar()`. Vive lo que vive WirePlumber. Está acá porque las pruebas
 * la necesitan, y para que el que lea la firma no lo busque en `modulo.c`.
 */
void vasak_medios_decisiones_libre (VasakMediosDecisiones *decisiones);

/**
 * Qué permisos tiene un cliente sobre un objeto de cámara.
 *
 * `por_omision` es lo que concede el gestor por omisión, y se devuelve tal
 * cual **sólo** si la decisión guardada es `PERMITIDA`. Cualquier otra cosa da
 * cero, que es no dar permiso.
 *
 * Lo que no está en la tabla vale cero, que es `NEGADA`. Eso cubre los tres
 * casos que importan: el cliente cuya consulta todavía no volvió, aquel cuya
 * consulta falló, y el que nunca llegó a preguntarse. Mientras no sepamos que
 * sí, es que no.
 */
guint32 vasak_medios_decisiones_permiso (VasakMediosDecisiones *decisiones,
                                         gpointer cliente,
                                         guint32 por_omision);

/**
 * La decisión guardada para un cliente.
 *
 * Si no hay nada guardado devuelve `VASAK_DECISION_NEGADA`. Lo ausente y lo
 * negado se responden igual, y a propósito: es el mismo cero que el de
 * `servicio.h`, y por eso no hace falta que la tabla distinga una cosa de la
 * otra para poder decir «no».
 *
 * Sirve, además, para probar las tres decisiones una por una sin pasar por
 * `…_permiso()` y sin que el `por_omision` tape lo que se está mirando.
 */
VasakDecision vasak_medios_decisiones_consultar (VasakMediosDecisiones *decisiones,
                                                 gpointer cliente);

/**
 * Anota la decisión que respondió el servicio, pisando lo que hubiera.
 *
 * Pisar es lo correcto y no un descuido: la respuesta del servicio es la
 * última palabra, y el `NEGADA` que se puso al preguntar era sólo un valor
 * inicial para que el cliente no tuviera la cámara mientras tanto.
 */
void vasak_medios_decisiones_anotar (VasakMediosDecisiones *decisiones,
                                     gpointer cliente, VasakDecision decision);

/**
 * Deja al cliente anotado como `NEGADA` si todavía no lo estaba, y dice si lo
 * hizo.
 *
 * El `TRUE` es lo que le dice a quien llama que puede enganchar su
 * `g_object_weak_ref`: se engancha una sola vez, la primera. Por eso esto
 * existe como función y no como dos llamadas sueltas en el llamador — la regla
 * «se marca una sola vez» queda escrita en un solo lugar, y no depende de que
 * quien llame se acuerde del `contains`.
 */
gboolean vasak_medios_decisiones_marcar_negada (VasakMediosDecisiones *decisiones,
                                                 gpointer cliente);

/**
 * Saca la entrada de un cliente. Para cuando el objeto cliente muere.
 *
 * Si no hay entrada no pasa nada, y se puede llamar de más.
 */
void vasak_medios_decisiones_olvidar (VasakMediosDecisiones *decisiones,
                                      gpointer cliente);

/** Vacía la tabla entera, sin destruirla. */
void vasak_medios_decisiones_limpiar (VasakMediosDecisiones *decisiones);
