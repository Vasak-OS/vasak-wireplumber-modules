#!/usr/bin/env bash
#
# Que el enganche de conexión pregunte con `QueryPermissionFor`.
#
# ── Por qué esto es una prueba de fuente y no una de C ──────────────────────
#
# Porque el método elegido es un **literal en un archivo que no se puede
# compilar en CI**: `modulo.c` incluye `<wp/wp.h>` y `<pipewire/permission.h>`,
# y los encabezados de WirePlumber 0.5 no están en los repos de Ubuntu. No hay
# forma de instanciar `VasakPermisosMedios` sin un servidor de PipeWire, y por
# lo tanto ninguna forma de observar a quién pregunta.
#
# Es una limitación real, no una comodidad: la elección entre los dos métodos es
# exactamente la clase de cosa que rompe en silencio, porque los dos compilan y
# los dos responden. Al revés de lo que suele pasar, donde una prueba de fuente
# es el plan B porque no se puede probar la unidad, acá el comportamiento no es
# una unidad: es una cadena de literales entre `al_seleccionar_acceso()` y la
# llamada por D-Bus. Lo que se rompe es el **nombre del método**, y un nombre es
# un literal. Esta prueba mira el literal.
#
# Y el fallo que nos trajo es un literal equivocado, medido: el módulo se
# encendió en el PR #9 llamando a `CheckPermissionFor`, que **no anota el
# intento ni avisa** —su respuesta a una decisión desconocida es abrir un
# diálogo con `agent::ask()` y, si nadie contesta, devolver `false` sin dejar
# rastro—. Con eso el paquete queda bloqueando la cámara sin registrar a la
# aplicación, sin aviso, y sin ningún lado donde darle permiso: exactamente el
# callejón sin salida por el que este módulo viajó apagado desde el primer día.
# `QueryPermissionFor` es la que llama a `anotar_y_avisar()`.
#
# Uso: pruebas/el-metodo-de-la-consulta.sh
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

FUENTE=src/permisos-de-medios/modulo.c
fallos=0
# `return 0` explícito, y a propósito: el estado que decide si la prueba pasó
# es el `exit` del final, que mira `$fallos`, no el que devuelven estas dos.
ok()  { printf '  \033[32m✓\033[0m %s\n' "$1"; return 0; }
mal() { printf '  \033[31m✗\033[0m %s\n' "$1"; fallos=$((fallos + 1)); return 0; }

# El cuerpo de una función, en el **código sin comentarios**: sacar los
# comentarios no es un detalle, es lo que permite que la explicación de por qué
# no se usa el otro método esté escrita al lado sin que esta prueba la lea como
# si fuera una llamada. Está en `pruebas/sin-comentarios.awk`.
codigo=$(awk -f pruebas/sin-comentarios.awk "$FUENTE")

# Se corta en la primera llave que cierra en la columna cero, que en el estilo de
# este archivo es la de la función: las de los `if` de adentro van sangradas. Y el
# nombre se busca con sangría admitida porque el filtro la conserva —por qué, en
# `pruebas/sin-comentarios.awk`—. Con eso `enviar_consulta (` no matchea
# `enviar_consulta_check (`.
cuerpo () {
    awk -v fn="$1" '
        $0 ~ "^[[:space:]]*" fn " *\\(" { dentro = 1 }
        dentro { print }
        dentro && $0 == "}" { exit }
    ' <<<"$codigo"
}

# Que el filtro no se haya comido el archivo. Un `grep` sobre un cuerpo vacío no
# falla nunca, así que si `sin-comentarios.awk` se rompiera, todo lo de abajo
# pasaría en verde sin haber comprobado nada.
if ! grep -q 'enviar_consulta' <<<"$codigo"; then
    mal "sin-comentarios.awk no devolvió nada de $FUENTE: lo de abajo no miraría nada"
fi

# Y que la cadena siga entera. Si el enganche dejara de llamar a `preguntar_por()`,
# o ésta de llamar a la consulta, las comprobaciones de abajo no mirarían la
# mitad de lo que dicen mirar.
enganche=$(cuerpo al_seleccionar_acceso)
if grep -qE '^[[:space:]]*preguntar_por \(' <<<"$enganche"; then
    ok "el enganche de conexión sigue preguntando por el cliente"
else
    mal "al_seleccionar_acceso() no llama a preguntar_por(): esta prueba no estaría mirando nada"
fi

pregunta=$(cuerpo preguntar_por)
if [[ -z "$pregunta" ]]; then
    mal "no se encontró preguntar_por() en $FUENTE"
elif grep -qE '^[[:space:]]*enviar_consulta \(' <<<"$pregunta"; then
    ok "y la consulta sale por enviar_consulta(), que es la de QueryPermissionFor"
else
    mal "preguntar_por() no manda por enviar_consulta(): la consulta va por otro camino"
fi

if grep -q 'enviar_consulta_check' <<<"$pregunta"; then
    mal "preguntar_por() manda por enviar_consulta_check(): se pierde el aviso y el registro del intento"
fi

consulta=$(cuerpo enviar_consulta)
if [[ -z "$consulta" ]]; then
    mal "no se encontró enviar_consulta() en $FUENTE"
else
    if grep -q 'VASAK_METODO_QUERY' <<<"$consulta"; then
        ok "que nombra VASAK_METODO_QUERY"
    else
        mal "enviar_consulta() no nombra VASAK_METODO_QUERY: la firma dice otra cosa"
    fi

    if grep -q 'VASAK_METODO_CHECK' <<<"$consulta"; then
        mal "enviar_consulta() nombra VASAK_METODO_CHECK: se abre un diálogo por cada conexión"
    fi

    # La firma también. QueryPermissionFor es (uts) -> (s) y CheckPermissionFor
    # es (utsu) -> (b): cambiar el nombre sin cambiar la firma deja una llamada
    # que el servicio responde con «Unknown method», y el módulo —que falla
    # cerrando— le niega la cámara a todo el mundo sin decir por qué.
    if grep -q '"(uts)"' <<<"$consulta" && grep -q '"(s)"' <<<"$consulta"; then
        ok "y con la firma de ese método: (uts) y (s)"
    else
        mal "la firma no es la de QueryPermissionFor: tiene que ser (uts) y (s)"
    fi
fi

printf '\n'
if [[ "$fallos" -eq 0 ]]; then
    printf '\033[32mSin fallos.\033[0m\n'
else
    printf '\033[31m%s fallo(s).\033[0m\n' "$fallos"
fi
exit $(( fallos > 0 ? 1 : 0 ))
