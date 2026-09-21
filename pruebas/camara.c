/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Qué cuenta como cámara y qué no.
 *
 * Las dos direcciones importan y ninguna falla a la vista: una regla de más
 * deja al escritorio sin compartir pantalla, una de menos deja la cámara
 * abierta.
 */

#include "camara.h"
#include <stdio.h>

static int fallos = 0;

static void
probar (const char *caso, const char *clase, const char *rol, const char *api,
        gboolean esperado)
{
  gboolean dio = vasak_medios_es_camara (clase, rol, api);
  printf ("  %s %s\n", dio == esperado ? "\033[32mok\033[0m"
                                       : "\033[31mMAL\033[0m", caso);
  if (dio != esperado) {
    printf ("      esperaba %s y dio %s\n",
            esperado ? "cámara" : "no cámara", dio ? "cámara" : "no cámara");
    fallos++;
  }
}

int
main (void)
{
  printf ("\nlo que es cámara\n");
  probar ("el nodo de una webcam", "Video/Source", "Camera", "v4l2", TRUE);
  probar ("el nodo, sin device.api", "Video/Source", "Camera", NULL, TRUE);
  probar ("el dispositivo v4l2", "Video/Device", NULL, "v4l2", TRUE);
  probar ("el bucle v4l2 de vasak-connect", "Video/Device", NULL, "v4l2", TRUE);

  printf ("\ny lo que no, que es la mitad que rompe callado\n");
  probar ("un flujo de captura de pantalla",
          "Stream/Output/Video", NULL, NULL, FALSE);
  probar ("un flujo de video cualquiera",
          "Stream/Input/Video", NULL, NULL, FALSE);
  probar ("un Video/Source que no es cámara",
          "Video/Source", NULL, NULL, FALSE);
  probar ("un Video/Source con otro rol",
          "Video/Source", "Screen", NULL, FALSE);
  probar ("un Video/Device que no es v4l2",
          "Video/Device", NULL, "libcamera", FALSE);
  probar ("un Video/Device sin device.api",
          "Video/Device", NULL, NULL, FALSE);
  probar ("el micrófono", "Audio/Source", NULL, "alsa", FALSE);
  probar ("los parlantes", "Audio/Sink", NULL, "alsa", FALSE);
  probar ("un objeto sin media.class", NULL, "Camera", "v4l2", FALSE);

  printf ("\n%s\n", fallos == 0 ? "\033[32mSin fallos.\033[0m"
                                : "\033[31mHay fallos.\033[0m");
  return fallos == 0 ? 0 : 1;
}
