/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Lo que eligió la persona, y cómo se guarda.
 *
 * ── Qué se guarda ───────────────────────────────────────────────────────────
 *
 * Tres cosas: si está encendido, qué perfil está elegido, y los valores del
 * perfil propio. Los de fábrica no se guardan —salen de `presets.c`—, así que
 * cambiar un perfil de fábrica en una versión nueva le llega a quien lo tenga
 * elegido.
 *
 * ── Dónde ───────────────────────────────────────────────────────────────────
 *
 * En `$XDG_STATE_HOME/vasak/equalizer.ini` (por omisión
 * `~/.local/state/vasak/equalizer.ini`), un archivo propio y no `vasak.conf`:
 * lo lee el módulo de WirePlumber, que arranca antes que cualquier aplicación y
 * no tiene por qué depender de una. Es estado y no configuración —lo cambia el
 * uso, no se escribe a mano—, y por eso va en `XDG_STATE_HOME`, que es donde
 * WirePlumber guarda también la salida por omisión que se eligió.
 *
 * ── Un archivo roto no rompe el audio ───────────────────────────────────────
 *
 * Lo que no se entiende se reemplaza por lo de fábrica, campo por campo: un
 * perfil que no existe vuelve a «flat», una ganancia fuera de rango vuelve a 0.
 * Lo que sí se entiende se conserva. Nunca se deja de arrancar por el archivo.
 */
#pragma once

#include <glib.h>

#include "presets.h"

G_BEGIN_DECLS

typedef struct {
  gboolean enabled;
  /* Uno de fábrica o «custom». Siempre válido: lo garantizan las funciones de
   * abajo, que son las únicas que lo escriben. */
  gchar preset[16];
  gdouble custom[VASAK_EQ_BANDS];
} VasakEqState;

/* Encendido, en «flat», y el propio en cero. */
void vasak_eq_state_defaults (VasakEqState *state);

/*
 * Lee el archivo. Si no existe, deja lo de fábrica y devuelve TRUE: es el
 * primer arranque, no un error. Si existe y no se puede leer o no es un
 * archivo de claves, deja lo de fábrica y devuelve FALSE con `error`. Si se lee
 * pero algún campo no sirve, se queda con lo que sí y devuelve TRUE.
 */
gboolean vasak_eq_state_load (VasakEqState *state, const gchar *path,
                              GError **error);

/* Lo escribe de una vez —archivo temporal y renombre—, creando el directorio
 * si hace falta. Un corte a mitad de camino deja el anterior entero. */
gboolean vasak_eq_state_save (const VasakEqState *state, const gchar *path,
                              GError **error);

/* `$XDG_STATE_HOME/vasak/equalizer.ini`. Se libera con g_free(). */
gchar *vasak_eq_state_default_path (void);

/* Las ganancias que suenan: las del perfil elegido, o las propias. */
void vasak_eq_state_gains (const VasakEqState *state,
                           gdouble out[VASAK_EQ_BANDS]);

/*
 * Mover una banda. Lo que suena pasa a ser el perfil propio: si estaba en
 * «rock», el propio toma los valores de «rock» y después se mueve esa banda,
 * que es lo que la persona ve en pantalla. Devuelve FALSE —y no cambia nada—
 * si la banda o la ganancia no son válidas.
 */
gboolean vasak_eq_state_set_gain (VasakEqState *state, guint band,
                                  gdouble gain);

/* Las diez de una vez, con la misma regla. FALSE si alguna no es válida, y en
 * ese caso no se aplica ninguna. */
gboolean vasak_eq_state_set_gains (VasakEqState *state,
                                   const gdouble gains[VASAK_EQ_BANDS]);

/* Elegir un perfil. «custom» vuelve a los valores propios guardados. FALSE si
 * no existe. */
gboolean vasak_eq_state_set_preset (VasakEqState *state, const gchar *id);

/* Si dos estados son el mismo, para saber si lo que está en disco es lo que
 * suena. */
gboolean vasak_eq_state_equal (const VasakEqState *a, const VasakEqState *b);

G_END_DECLS
