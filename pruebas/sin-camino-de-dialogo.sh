#!/usr/bin/env bash
#
# Que no vuelva a existir un camino vivo que pregunte con `CheckPermissionFor`.
#
# ── Qué ata esta prueba y por qué no alcanza con la anterior ────────────────
#
# `el-metodo-de-la-consulta.sh` mira **el enganche**: que la cadena que va de
# `al_seleccionar_acceso()` a la llamada por D-Bus termine en
# `QueryPermissionFor`. Esta mira **el archivo**: que el nombre del método que
# abre el diálogo no aparezca en ningún código que se pueda llegar a ejecutar.
#
# Son dos preguntas distintas y las dos hacen falta. La anterior puede pasar
# con `enviar_consulta_check()` sentada al lado, sin usar, y lo que evita es que
# el enganche vuelva a elegir mal. Esta no deja que exista el camino: si alguien
# agrega un segundo consulto, o deja la constante a mano «por si acaso», ya no
# hay dónde colgarlo.
#
# El que abre el diálogo es `CheckPermissionFor`: ante una decisión que no
# conoce llama a `agent::ask()`. El módulo de WirePlumber ve a cada cliente
# **al conectarse**, y «¿le permitís la cámara a pactl?» no es una pregunta que
# alguien pueda contestar —no hay nadie mirando una pantalla de permisos en el
# momento en que arranca el navegador—. Preguntarla en cada conexión enseña a
# conceder sin leer, que es justo lo que `vasak-permissions` existe para no
# hacer, y mientras tanto el intento queda sin anotar y la aplicación sin
# aparecer en Privacidad y seguridad: bloqueada, y sin forma de desbloquearla.
#
# Uso: pruebas/sin-camino-de-dialogo.sh
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

fallos=0
ok()  { printf '  \033[32m✓\033[0m %s\n' "$1"; return 0; }
mal() { printf '  \033[31m✗\033[0m %s\n' "$1"; fallos=$((fallos + 1)); return 0; }

MODULO=src/permisos-de-medios/modulo.c
codigo=$(awk -f pruebas/sin-comentarios.awk "$MODULO")

# Que el filtro no se haya comido el archivo entero: una guarda que desaparece
# por un error propio pasa en verde sin haber comprobado nada, que es el modo de
# fallar de esta prueba hacia adentro.
if [[ -z "$codigo" ]]; then
    mal "sin-comentarios.awk no devolvió nada de $MODULO: lo de abajo no miraría nada"
elif ! grep -q 'enviar_consulta' <<<"$codigo"; then
    mal "el código de $MODULO no tiene ninguna consulta: lo de abajo no miraría nada"
fi

# En el código de `modulo.c` no puede aparecer el literal, en ninguna forma: ni
# la cadena suelta ni la constante que la envolvería. Las dos compilan igual de
# bien, que es el problema.
#
# Los comentarios quedan afuera a propósito, y el porqué está en
# `pruebas/sin-comentarios.awk`: en este repositorio está escrito por qué **no**
# usar el otro método, y una puerta que se activara con esa explicación obligaría
# a borrarla.
if [[ -n "$codigo" ]]; then
    if grep -E 'CheckPermissionFor|VASAK_METODO_CHECK' <<<"$codigo" >/dev/null; then
        while IFS= read -r linea; do
            mal "$MODULO lo llama: $linea"
        done < <(grep -E 'CheckPermissionFor|VASAK_METODO_CHECK' <<<"$codigo")
    else
        ok "$MODULO no llama al método que abre el diálogo"
    fi
fi

# Y la constante tampoco queda huérfana en el contrato: una constante de método
# que nadie llama es la forma que tomó el camino de volver a colgarlo, porque el
# enganche la tiene a dos líneas de distancia.
if grep -q '^#define VASAK_METODO_CHECK' src/permisos-de-medios/servicio.h; then
    mal "servicio.h define VASAK_METODO_CHECK y nada la llama"
else
    ok "servicio.h no deja la constante del método que abre el diálogo"
fi

# Tampoco queda el traductor de su respuesta, que es un `? :` sobre un
# booleano: código sin una sola llamada, en una unidad que el CI sí compila, y
# por lo tanto sin ninguna prueba que lo pueda cubrir.
if grep -q 'vasak_medios_check_desde_booleano' \
     src/permisos-de-medios/servicio.c \
     src/permisos-de-medios/servicio.h \
     src/permisos-de-medios/modulo.c; then
    mal "queda el traductor de la respuesta booleana, que ya no contesta nadie"
else
    ok "y no queda el traductor de la respuesta booleana"
fi

# Lo que sí tiene que estar, para que la prueba de arriba no pueda pasar por un
# motivo equivocado: el método que se usa, nombrado en el contrato.
if grep -q '^#define VASAK_METODO_QUERY .*"QueryPermissionFor"' \
     src/permisos-de-medios/servicio.h; then
    ok "el contrato sigue declarando el método que sí se usa"
else
    mal "servicio.h no declara VASAK_METODO_QUERY: el contrato se quedó sin método"
fi

printf '\n'
if [[ "$fallos" -eq 0 ]]; then
    printf '\033[32mSin fallos.\033[0m\n'
else
    printf '\033[31m%s fallo(s).\033[0m\n' "$fallos"
fi
exit $(( fallos > 0 ? 1 : 0 ))
