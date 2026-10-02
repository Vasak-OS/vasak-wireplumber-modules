/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Que los dos fragmentos digan lo mismo que la tabla de `presets.c`.
 *
 * Son tres archivos que no se hablan —el grafo de PipeWire, el componente de
 * WirePlumber y el código— y separarlos no falla en ninguno: una banda con
 * otro nombre en el grafo hace que PipeWire ignore su ganancia sin decir nada,
 * un filtro sin `filter.smart` se queda enganchado a la primera salida y no la
 * sigue, y un componente con otro nombre no carga nunca. Tres maneras de que
 * el ecualizador «no haga nada» sin una sola queja en el registro.
 *
 * Las rutas llegan por variables de entorno desde `meson.build`.
 */

#include "presets.h"

static gchar *
read_env_file (const gchar *variable)
{
  const gchar *path = g_getenv (variable);
  g_assert_nonnull (path);
  gchar *contents = NULL;
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  return contents;
}

/* Sin los comentarios: lo que se busca no puede aparecer sólo en uno. */
static gchar *
strip_comments (const gchar *text)
{
  g_autoptr (GRegex) comment = g_regex_new ("#[^\\n]*", 0, 0, NULL);
  return g_regex_replace (comment, text, -1, 0, "", 0, NULL);
}

static gboolean
matches (const gchar *text, const gchar *pattern)
{
  return g_regex_match_simple (pattern, text, G_REGEX_MULTILINE, 0);
}

static void
test_bands_in_graph (void)
{
  g_autofree gchar *raw = read_env_file ("EQ_PIPEWIRE_CONF");
  g_autofree gchar *conf = strip_comments (raw);

  for (guint i = 0; i < VASAK_EQ_BANDS; i++) {
    /* name = eq_31 label = bq_peaking
     *   control = { "Freq" = 31.0 … "Gain" = 0.0 } */
    g_autofree gchar *pattern = g_strdup_printf (
        "name\\s*=\\s*%s\\s+label\\s*=\\s*bq_peaking\\s+"
        "control\\s*=\\s*\\{\\s*\"Freq\"\\s*=\\s*%g(\\.0)?\\s+"
        "\"Q\"\\s*=\\s*[0-9.]+\\s+\"Gain\"\\s*=\\s*0(\\.0)?\\s*\\}",
        vasak_eq_band_node (i), vasak_eq_band_frequency (i));
    if (!matches (conf, pattern))
      g_error ("el grafo no tiene la banda %s en %g Hz, plana",
               vasak_eq_band_node (i), vasak_eq_band_frequency (i));
  }

  /* El preamplificador: un estante agudo con Freq = 0 es ganancia plana. */
  g_assert_true (matches (conf,
      "name\\s*=\\s*" VASAK_EQ_PREAMP_NODE "\\s+label\\s*=\\s*bq_highshelf\\s+"
      "control\\s*=\\s*\\{\\s*\"Freq\"\\s*=\\s*0(\\.0)?\\s"));
}

static void
test_chain_is_complete (void)
{
  g_autofree gchar *raw = read_env_file ("EQ_PIPEWIRE_CONF");
  g_autofree gchar *conf = strip_comments (raw);

  /* preamp → eq_31 → … → eq_16k, sin saltearse ninguna. Una banda sin enlace
   * queda como entrada o salida suelta del grafo, y filter-chain la toma como
   * otro canal. */
  const gchar *previous = VASAK_EQ_PREAMP_NODE;
  for (guint i = 0; i < VASAK_EQ_BANDS; i++) {
    g_autofree gchar *pattern = g_strdup_printf (
        "output\\s*=\\s*\"%s:Out\"\\s+input\\s*=\\s*\"%s:In\"",
        previous, vasak_eq_band_node (i));
    if (!matches (conf, pattern))
      g_error ("falta el enlace %s → %s", previous, vasak_eq_band_node (i));
    previous = vasak_eq_band_node (i);
  }
}

static void
test_filter_is_smart (void)
{
  g_autofree gchar *raw = read_env_file ("EQ_PIPEWIRE_CONF");
  g_autofree gchar *conf = strip_comments (raw);

  /* El lado que recibe: se llama como lo busca el módulo, es una salida para
   * las aplicaciones, y es inteligente — que es lo que hace que siga a la
   * salida por omisión. */
  g_autoptr (GRegex) block = g_regex_new ("capture\\.props\\s*=\\s*\\{[^}]*\\}",
                                          0, 0, NULL);
  g_autoptr (GMatchInfo) info = NULL;
  g_assert_true (g_regex_match (block, conf, 0, &info));
  g_autofree gchar *capture = g_match_info_fetch (info, 0);

  g_assert_true (matches (capture,
      "node\\.name\\s*=\\s*\"" VASAK_EQ_FILTER_NODE "\""));
  g_assert_true (matches (capture, "media\\.class\\s*=\\s*Audio/Sink"));
  g_assert_true (matches (capture, "filter\\.smart\\s*=\\s*true"));
  /* Sin destino: si tuviera `filter.smart.target` quedaría atado a esa salida
   * y no seguiría a la que se elija. */
  g_assert_false (matches (capture, "filter\\.smart\\.target"));

  /* Y la salida del filtro, pasiva: si no, mantiene despierto al dispositivo
   * aunque no suene nada. */
  g_assert_true (matches (conf, "node\\.passive\\s*=\\s*true"));
}

static void
test_wireplumber_component (void)
{
  g_autofree gchar *raw = read_env_file ("EQ_WIREPLUMBER_CONF");
  g_autofree gchar *conf = strip_comments (raw);
  g_autofree gchar *meson = read_env_file ("EQ_MESON_BUILD");

  g_assert_true (matches (conf,
      "name\\s*=\\s*libwireplumber-module-vasak-equalizer,\\s*type\\s*=\\s*module"));
  /* Que viaje encendido, y que espere al metadato donde se lo apaga. */
  g_assert_true (matches (conf, "custom\\.vasak-equalizer\\s*=\\s*required"));
  g_assert_true (matches (conf, "requires\\s*=\\s*\\[\\s*metadata\\.filters\\s*\\]"));
  /* Que el nombre sea el que compila meson. */
  g_assert_true (matches (meson,
      "shared_module\\('wireplumber-module-vasak-equalizer'"));
  /* Y que el propio fragmento diga cómo apagarlo. */
  g_assert_true (matches (raw, "custom\\.vasak-equalizer = disabled"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/config/las diez bandas están en el grafo, planas",
                   test_bands_in_graph);
  g_test_add_func ("/config/la cadena pasa por todas, en orden",
                   test_chain_is_complete);
  g_test_add_func ("/config/el filtro es inteligente y sin destino",
                   test_filter_is_smart);
  g_test_add_func ("/config/el componente se llama como el módulo",
                   test_wireplumber_component);
  return g_test_run ();
}
