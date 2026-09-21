/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Cómo se traduce la respuesta del servicio, que es la línea donde un error se
 * convierte en «pasá».
 *
 * Todo lo que no sea exactamente `allowed` tiene que ser negado: una respuesta
 * que este módulo no conozca, una vacía, un `NULL` porque la llamada falló. La
 * dirección importa en un solo sentido — equivocarse hacia negar deja a alguien
 * sin cámara y se nota; equivocarse hacia permitir no se nota nunca.
 */

#include "servicio.h"
#include <stdio.h>

static int fallos = 0;

static void
probar (const char *caso, const char *texto, VasakDecision esperada)
{
  VasakDecision dio = vasak_medios_decision_desde_texto (texto);
  printf ("  %s %s\n", dio == esperada ? "\033[32mok\033[0m"
                                       : "\033[31mMAL\033[0m", caso);
  if (dio != esperada) {
    printf ("      esperaba %d y dio %d\n", esperada, dio);
    fallos++;
  }
}

int
main (void)
{
  printf ("\nlas tres respuestas que el servicio da hoy\n");
  probar ("allowed", "allowed", VASAK_DECISION_PERMITIDA);
  probar ("denied", "denied", VASAK_DECISION_NEGADA);
  probar ("unknown no es permitido", "unknown", VASAK_DECISION_SIN_DECIDIR);

  printf ("\ny todo lo demás niega\n");
  probar ("una cadena vacía", "", VASAK_DECISION_NEGADA);
  probar ("NULL, o sea que la llamada falló", NULL, VASAK_DECISION_NEGADA);
  probar ("una respuesta futura que no conocemos", "ask-later",
          VASAK_DECISION_NEGADA);
  probar ("con mayúsculas no cuenta", "Allowed", VASAK_DECISION_NEGADA);
  probar ("con espacios tampoco", " allowed", VASAK_DECISION_NEGADA);
  probar ("algo que la contiene", "not-allowed", VASAK_DECISION_NEGADA);

  printf ("\ny el cero de la tabla es negar\n");
  probar ("lo que no está en la tabla vale cero",
          "denied", (VasakDecision) 0);

  printf ("\n%s\n", fallos == 0 ? "\033[32mSin fallos.\033[0m"
                                : "\033[31mHay fallos.\033[0m");
  return fallos == 0 ? 0 : 1;
}
