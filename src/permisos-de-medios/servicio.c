/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "servicio.h"

VasakDecision
vasak_medios_decision_desde_texto (const gchar *texto)
{
  if (g_strcmp0 (texto, "allowed") == 0)
    return VASAK_DECISION_PERMITIDA;

  if (g_strcmp0 (texto, "unknown") == 0)
    return VASAK_DECISION_SIN_DECIDIR;

  /* `denied`, NULL, y cualquier cosa que no conozcamos. Que el caso por
   * omisión sea negar no es prolijidad: si un día el servicio agrega una
   * respuesta nueva, este módulo la va a leer sin entenderla, y lo único
   * aceptable entonces es no dar la cámara. */
  return VASAK_DECISION_NEGADA;
}
