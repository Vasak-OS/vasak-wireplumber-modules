/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * La interfaz D-Bus del ecualizador: `org.vasak.Equalizer1`, en
 * `/org/vasak/Equalizer`, con el nombre `org.vasak.Equalizer` en el bus de la
 * sesión.
 *
 * El contrato completo —propiedades, métodos, errores y qué hace la interfaz
 * gráfica con cada uno— está escrito en el README, sección «La interfaz
 * D-Bus». Lo que va acá es cómo está armado por dentro.
 *
 * ── Por qué está aparte del módulo ──────────────────────────────────────────
 *
 * Por lo mismo que `decisiones.c` en el otro módulo: esto es lo que la
 * interfaz gráfica ve, y se tiene que poder probar sin un WirePlumber andando.
 * Sólo incluye GIO, y las pruebas lo ejercitan con un bus privado
 * (`GTestDBus`) y un cliente de verdad.
 *
 * El servicio no sabe nada de PipeWire: cuando algo cambia llama a `apply`, y
 * quien lo creó —`module.c`— le pone las ganancias al nodo del filtro. Y quien
 * lo creó le avisa con `…_set_available()` si el nodo existe.
 */
#pragma once

#include <gio/gio.h>

#include "state.h"

G_BEGIN_DECLS

#define VASAK_EQ_BUS_NAME "org.vasak.Equalizer"
#define VASAK_EQ_OBJECT_PATH "/org/vasak/Equalizer"
#define VASAK_EQ_INTERFACE "org.vasak.Equalizer1"

typedef struct _VasakEqService VasakEqService;

/* Lo que suena tiene que pasar a ser `state`. */
typedef void (*VasakEqApplyFunc) (const VasakEqState *state,
                                  gpointer user_data);

/*
 * Un servicio nuevo, con el estado leído de `state_path`. `save_delay_ms` es
 * cuánto espera para escribir después del último cambio: arrastrar un
 * deslizador manda decenas de cambios por segundo, y escribir el archivo en
 * cada uno sería gastar el disco en valores que nadie va a recordar.
 */
VasakEqService *vasak_eq_service_new (const gchar *state_path,
                                      guint save_delay_ms,
                                      VasakEqApplyFunc apply,
                                      gpointer user_data);

/* Escribe lo pendiente, si hay, y lo libera. Acepta NULL. */
void vasak_eq_service_free (VasakEqService *self);

/* Publica el objeto en esa conexión. No toma el nombre: eso lo hace quien
 * tiene la conexión, que sabe cuándo la tiene. */
gboolean vasak_eq_service_export (VasakEqService *self,
                                  GDBusConnection *connection,
                                  GError **error);

void vasak_eq_service_unexport (VasakEqService *self);

/* Si el nodo del filtro está en PipeWire. Sin él, cambiar algo se guarda
 * igual y suena cuando el nodo aparezca. */
void vasak_eq_service_set_available (VasakEqService *self, gboolean available);

const VasakEqState *vasak_eq_service_get_state (const VasakEqService *self);

/* Escribe ya lo que esté pendiente. Las pruebas lo usan; el módulo, al
 * apagarse. */
void vasak_eq_service_flush (VasakEqService *self);

G_END_DECLS
