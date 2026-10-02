/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Las bandas y los perfiles del ecualizador: la única tabla de la que salen.
 *
 * Las frecuencias y los nombres de los nodos están también en
 * `data/60-vasak-equalizer.conf`, que es el grafo que carga PipeWire. Son dos
 * archivos que no se hablan, y separarlos no falla: un nombre que no existe en
 * el grafo hace que PipeWire ignore esa banda sin decir nada. Por eso
 * `tests/equalizer/config.c` lee el fragmento y lo compara con esta tabla.
 *
 * Nada de acá incluye WirePlumber: se compila y se prueba con glib pelado.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define VASAK_EQ_BANDS 10

/* El rango que acepta cada banda, en dB. Doce es lo que traen la mayoría de
 * los ecualizadores gráficos, y alcanza de sobra para lo que es corregir unos
 * auriculares; más que eso es pedirle al preamplificador que baje el resto otro
 * tanto. */
#define VASAK_EQ_GAIN_MIN (-12.0)
#define VASAK_EQ_GAIN_MAX (12.0)

/* El perfil propio: no está en la tabla de fábrica, sus valores los pone la
 * persona y se guardan aparte. */
#define VASAK_EQ_PRESET_CUSTOM "custom"
#define VASAK_EQ_PRESET_FLAT "flat"

typedef struct {
  /* Identificador estable, en inglés y en minúsculas: es lo que viaja por
   * D-Bus y se guarda en disco. El nombre que se ve lo traduce la interfaz. */
  const gchar *id;
  gdouble gains[VASAK_EQ_BANDS];
} VasakEqPreset;

/* La frecuencia central de una banda, en Hz. */
gdouble vasak_eq_band_frequency (guint band);

/* El nombre del nodo del grafo de esa banda («eq_31», «eq_1k»…). */
const gchar *vasak_eq_band_node (guint band);

/* El nodo de la ganancia plana que va delante de las bandas. */
#define VASAK_EQ_PREAMP_NODE "preamp"

/* El `node.name` del lado que recibe el audio —el `Audio/Sink`—, que es el que
 * WirePlumber trata como el filtro y el que recibe los `Props`. */
#define VASAK_EQ_FILTER_NODE "vasak-equalizer"

/* Los perfiles de fábrica, en el orden en que se muestran. */
const VasakEqPreset *vasak_eq_presets (guint *n_presets);

/* Un perfil de fábrica por su id, o NULL. «custom» no es de fábrica. */
const VasakEqPreset *vasak_eq_preset_find (const gchar *id);

/* Si `id` es un perfil que se puede elegir: uno de fábrica o «custom». */
gboolean vasak_eq_preset_id_valid (const gchar *id);

/* Que sea un número, y que esté dentro del rango. NaN e infinito no lo son. */
gboolean vasak_eq_gain_valid (gdouble gain);

/*
 * La ganancia del preamplificador para un juego de bandas: menos la banda más
 * alta, si alguna sube, y cero si ninguna sube.
 *
 * Es lo que evita que subir los graves sature. El grafo trabaja en coma
 * flotante y no recorta, pero la placa de sonido sí, y una canción ya
 * masterizada al máximo con +6 dB en 63 Hz recorta en cada golpe de bombo.
 * Bajar el resto tanto como suba la banda más alta deja el pico donde estaba.
 */
gdouble vasak_eq_preamp (const gdouble gains[VASAK_EQ_BANDS]);

G_END_DECLS
