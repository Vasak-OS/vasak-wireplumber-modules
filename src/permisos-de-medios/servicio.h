/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib.h>

/* El contrato con `vasak-permissions`, en un solo lugar.
 *
 * Son cadenas de un servicio que se compila aparte y no comparte nada de este
 * código: un renombre del otro lado compila perfecto acá y deja al módulo
 * llamando a un método que no existe. Como el módulo falla cerrando, eso
 * apagaría la cámara para todos sin que nada diga por qué. Del lado del
 * servicio hay una constante y una prueba por cada uno de estos nombres.
 */
#define VASAK_SERVICIO   "ar.net.vasak.os.Permissions"
#define VASAK_RUTA       "/ar/net/vasak/os/Permissions"
#define VASAK_INTERFAZ   "ar.net.vasak.os.Permissions"
#define VASAK_METODO     "QueryPermissionFor"

/* El identificador del recurso, tal como se guarda en la política en disco. */
#define VASAK_RECURSO_CAMARA "camera"

/** Lo que el servicio contesta. */
typedef enum {
  /* Negado, y también todo lo que no se entienda: el orden importa porque el
   * cero es el estado inicial de la tabla. Un cliente del que todavía no
   * sabemos nada está negado. */
  VASAK_DECISION_NEGADA = 0,
  VASAK_DECISION_PERMITIDA,
  /* La persona todavía no decidió. **No es permitido**: acá se trata igual que
   * negado, y existe aparte sólo para poder anotarlo distinto en el registro.
   * Quien pregunta con contexto es el portal. */
  VASAK_DECISION_SIN_DECIDIR,
} VasakDecision;

/**
 * Traduce la respuesta del servicio.
 *
 * Todo lo que no sea exactamente `allowed` es negado, incluido `NULL` y
 * cualquier cadena futura que este módulo no conozca. Aparte para poder
 * probarlo: es la línea donde un error se convierte en «pasá».
 */
VasakDecision vasak_medios_decision_desde_texto (const gchar *texto);
