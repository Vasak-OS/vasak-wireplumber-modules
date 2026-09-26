/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Que el instante de arranque salga bien del `/proc/<pid>/stat`, incluso
 * cuando el nombre del ejecutable trae espacios y paréntesis.
 *
 * Esto no es una hipótesis: el kernel no escapa el nombre, y cualquiera puede
 * llamar a su programa `mi (raro) programa`. Un parser que busque el primer
 * `)` o parta por espacios devuelve otro número, y el error **no se ve**: el
 * servicio de permisos rechaza el proceso como si hubiera muerto y la cámara
 * queda negada sin explicación.
 */

#include "identidad.h"

#include <stdio.h>
#include <unistd.h>

static int fallos = 0;

static void
igual (const char *caso, gboolean ok, guint64 dio, gboolean ok_esperado,
       guint64 esperado)
{
  gboolean bien = (ok == ok_esperado) && (!ok_esperado || dio == esperado);
  printf ("  %s %s\n", bien ? "\033[32mok\033[0m" : "\033[31mMAL\033[0m", caso);
  if (!bien) {
    printf ("      esperaba ok=%d valor=%" G_GUINT64_FORMAT "\n",
            ok_esperado, esperado);
    printf ("      dio     ok=%d valor=%" G_GUINT64_FORMAT "\n", ok, dio);
    fallos++;
  }
}

static void
probar (const char *caso, const char *stat, gboolean ok_esperado,
        guint64 esperado)
{
  guint64 dio = 0;
  gboolean ok = vasak_medios_inicio_desde_stat (stat, &dio);
  igual (caso, ok, dio, ok_esperado, esperado);
}

int
main (void)
{
  printf ("\nel instante de arranque sale del campo 22\n");

  /* Un stat de verdad, recortado después del campo 22. Los campos 3..21 son
   * los que hay que saltar. */
  probar ("un proceso normal",
          "1234 (wireplumber) S 1 1234 1234 0 -1 4194304 1 0 0 0 1 2 0 0 "
          "20 0 5 0 987654 1 2 3\n",
          TRUE, 987654);

  printf ("\ny el nombre del ejecutable no lo arruina\n");

  probar ("con espacios en el nombre",
          "1234 (mi programa) S 1 1234 1234 0 -1 4194304 1 0 0 0 1 2 0 0 "
          "20 0 5 0 111 1 2 3\n",
          TRUE, 111);

  probar ("con paréntesis adentro",
          "1234 (mi (raro) programa) S 1 1234 1234 0 -1 4194304 1 0 0 0 1 2 "
          "0 0 20 0 5 0 222 1 2 3\n",
          TRUE, 222);

  probar ("terminando en paréntesis",
          "1234 (raro)) S 1 1234 1234 0 -1 4194304 1 0 0 0 1 2 0 0 "
          "20 0 5 0 333 1 2 3\n",
          TRUE, 333);

  printf ("\ny lo que no tiene la forma esperada se rechaza, no se adivina\n");

  probar ("sin paréntesis", "1234 wireplumber S 1 1234", FALSE, 0);
  probar ("cortado antes del campo 22", "1234 (wp) S 1 1234 1234\n", FALSE, 0);
  probar ("vacío", "", FALSE, 0);
  probar ("nulo", NULL, FALSE, 0);
  probar ("con basura donde va el número",
          "1234 (wp) S 1 1234 1234 0 -1 4194304 1 0 0 0 1 2 0 0 "
          "20 0 5 0 nada 1 2 3\n",
          FALSE, 0);

  printf ("\ny contra un proceso de verdad\n");
  {
    guint64 a = 0;
    guint64 b = 0;
    gboolean ok1 = vasak_medios_inicio_del_proceso (getpid (), &a);
    gboolean ok2 = vasak_medios_inicio_del_proceso (getpid (), &b);
    /* El mismo proceso tiene que dar siempre el mismo número: es lo que hace
     * que sirva para fijar la identidad contra la reutilización de pid. */
    igual ("el propio proceso, dos veces, da lo mismo",
           ok1 && ok2 && a == b, a, TRUE, a);

    guint64 c = 0;
    igual ("un pid que no existe se rechaza",
           vasak_medios_inicio_del_proceso (0x7FFFFFF0, &c), 0, FALSE, 0);
  }

  printf ("\n%s\n", fallos == 0 ? "\033[32mSin fallos.\033[0m"
                                : "\033[31mHay fallos.\033[0m");
  return fallos == 0 ? 0 : 1;
}
