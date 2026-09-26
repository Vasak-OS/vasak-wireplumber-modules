/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * La tabla de decisiones. Lo único de este módulo que decide, y lo único que
 * se puede probar sin un servidor de PipeWire andando.
 *
 * Nada de esto conoce `VasakPermisosMedios`: la tabla no sabe quién la guarda,
 * ni cuándo se consulta, ni qué es un `WpClient`. Recibe un `gpointer` y
 * devuelve permisos. Todo lo que necesita saber del mundo se lo pasa el
 * llamador.
 */

#include "decisiones.h"

struct _VasakMediosDecisiones
{
  /* objeto cliente -> `GUINT_TO_POINTER (decision)`.
   *
   * `g_direct_hash` y `g_direct_equal`: la clave se compara **por puntero** y
   * no por cadena, y el valor es un entero, no algo que haya que liberar. Por
   * eso la tabla no lleva función de liberación: lo único que hay que liberar
   * después es la tabla misma.
   *
   * Que `g_direct_equal` acepte `NULL` como clave importa: una decisión
   * `NEGADA` vale cero, y cero como puntero es `NULL`. No es un caso raro, es
   * el estado en el que está todo cliente que todavía no respondió. */
  GHashTable *tabla;
};

VasakMediosDecisiones *
vasak_medios_decisiones_nueva (void)
{
  VasakMediosDecisiones *decisiones = g_new0 (VasakMediosDecisiones, 1);
  decisiones->tabla = g_hash_table_new (g_direct_hash, g_direct_equal);
  return decisiones;
}

void
vasak_medios_decisiones_libre (VasakMediosDecisiones *decisiones)
{
  if (decisiones == NULL)
    return;

  g_hash_table_destroy (decisiones->tabla);
  g_free (decisiones);
}

VasakDecision
vasak_medios_decisiones_consultar (VasakMediosDecisiones *decisiones,
                                   gpointer cliente)
{
  g_return_val_if_fail (decisiones != NULL, VASAK_DECISION_NEGADA);

  /* El valor guardado es el entero de la decisión tal cual, y el cero de
   * `VasakDecision` es `NEGADA` — así que lo que no está sale igual que lo que
   * está negado, sin ninguna decisión de por medio. Es la misma aritmética que
   * hacía el enganche, con el nombre puesto.
   *
   * Ojo con la trampa al tocar esta línea: `NEGADA` se guarda como
   * `GUINT_TO_POINTER (0)`, o sea `NULL`, y `g_hash_table_lookup` devuelve
   * `NULL` tanto para «ausente» como para «negada». Acá no importa porque los
   * dos se responden igual —y por eso el valor de retorno no distingue
   * «ausente» de «negada», a propósito—. En `…_marcar_negada()` sí importa, y
   * por eso ahí va `g_hash_table_contains()` y no un lookup. */
  return (VasakDecision) GPOINTER_TO_UINT (
      g_hash_table_lookup (decisiones->tabla, cliente));
}

guint32
vasak_medios_decisiones_permiso (VasakMediosDecisiones *decisiones,
                                 gpointer cliente, guint32 por_omision)
{
  /* Un `NULL` acá es un error de programación, no un caso de ejecución: se avisa
   * con un `critical` y se devuelve cero. Fallar hacia negar es la dirección
   * correcta aun cuando lo que falla sea el propio módulo. */
  g_return_val_if_fail (decisiones != NULL, 0);

  /* Lo único que concede la cámara es una decisión guardada que sea
   * `PERMITIDA`. `NEGADA`, `SIN_DECIDIR` y que no haya nada dan cero, que es
   * no dar permiso: mientras no sepamos que sí, es que no. La pregunta todavía
   * en vuelo, la que falló y la que nunca se hizo quedan las tres igual. */
  if (vasak_medios_decisiones_consultar (decisiones, cliente) !=
      VASAK_DECISION_PERMITIDA)
    return 0;

  return por_omision;
}

void
vasak_medios_decisiones_anotar (VasakMediosDecisiones *decisiones,
                                gpointer cliente, VasakDecision decision)
{
  g_return_if_fail (decisiones != NULL);

  g_hash_table_insert (decisiones->tabla, cliente,
                       GUINT_TO_POINTER (decision));
}

gboolean
vasak_medios_decisiones_marcar_negada (VasakMediosDecisiones *decisiones,
                                       gpointer cliente)
{
  g_return_val_if_fail (decisiones != NULL, FALSE);

  /* `contains` y no un lookup, por lo de `NULL`: una decisión `NEGADA` está
   * guardada como `GUINT_TO_POINTER (0)`, así que el lookup no la distingue de
   * «ausente», y un cliente ya marcado se volvería a marcar — con lo que
   * además se le engancharía un segundo `g_object_weak_ref`.
   *
   * Las dos operaciones van juntas y no separadas en el llamador por dos
   * razones. La primera es que la regla «se marca una sola vez» quede escrita
   * en un lugar, y no dependa de que quien llama se acuerde del `contains` y
   * de que nadie inserte en el medio. La segunda es más chica: así el «¿ya
   * estaba?» y el «anotar» son una sola cosa, y el `TRUE` de acá es lo que le
   * dice al llamador que enganche la referencia débil.
   *
   * Lo que esto **no** es: una promesa de que la tabla sea segura entre hilos.
   * `GHashTable` no lo es, y el módulo corre todo en el hilo de WirePlumber.
   * Lo que cambia es que la secuencia ya no se puede dejar a medias. */
  if (g_hash_table_contains (decisiones->tabla, cliente))
    return FALSE;

  g_hash_table_insert (decisiones->tabla, cliente,
                       GUINT_TO_POINTER (VASAK_DECISION_NEGADA));
  return TRUE;
}

void
vasak_medios_decisiones_olvidar (VasakMediosDecisiones *decisiones,
                                 gpointer cliente)
{
  g_return_if_fail (decisiones != NULL);

  g_hash_table_remove (decisiones->tabla, cliente);
}

void
vasak_medios_decisiones_limpiar (VasakMediosDecisiones *decisiones)
{
  g_return_if_fail (decisiones != NULL);

  g_hash_table_remove_all (decisiones->tabla);
}
