#!/usr/bin/env bash
#
# Que el módulo viaje en el paquete **encendido**, y que el fragmento que lo
# declara se instale.
#
# Las dos mitades importan y ninguna sola alcanza:
#
#   · Un fragmento que no se instala deja el módulo en el disco y sin cargar
#     nunca, y nada lo dice. A este taller ya le pasó con un drop-in de GRUB
#     que estuvo escrito y sin empaquetar durante meses.
#   · Un fragmento que se instala **apagado** deja el permiso de cámara sin
#     hacer cumplir, y desde afuera se ve exactamente igual que uno que
#     funciona: la pantalla de Privacidad muestra los interruptores, la persona
#     los mueve, y no gobiernan nada. Es la mentira que este módulo existe para
#     no contar.
#
# Esta prueba estaba al revés hasta `vasak-permissions` 0.14.0, y a propósito:
# mientras una negación no generara aviso ni dejara a la aplicación listada,
# encenderlo era bloquear sin poder desbloquear. Se dio vuelta en el mismo
# commit que lo encendió, que es como estaba previsto.
#
# Uso: pruebas/viene-encendido.sh
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

FRAGMENTO=datos/50-vasak-permisos-de-medios.conf
fallos=0
# `return 0` explícito, y a propósito: el estado que decide si la prueba pasó
# es el `exit` del final, que mira `$fallos`, no el que devuelven estas dos.
# Hoy `ok` ya sale con 0 —el de su `printf`— y `mal` también, porque su última
# orden es una asignación y una asignación siempre tiene éxito. Devolver 1
# desde `mal`, que sería lo «correcto» en apariencia, no cambia el resultado de
# la prueba, pero convierte el estado de la rama en el de un fallo, que es
# justo lo que estas dos existen para registrar aparte.
ok()  { printf '  \033[32m✓\033[0m %s\n' "$1"; return 0; }
mal() { printf '  \033[31m✗\033[0m %s\n' "$1"; fallos=$((fallos + 1)); return 0; }

if [[ -f "$FRAGMENTO" ]]; then
    ok "el fragmento existe"
else
    mal "falta $FRAGMENTO"
fi

# Que se instale, **y dónde**. Las dos cosas: un fragmento instalado fuera de
# `wireplumber.conf.d` no lo descubre nadie, y desde acá se ve igual de bien que
# uno bien puesto. Lo marcó CodeRabbit en el PR #2.
# awk y no sed: la ruta del fragmento lleva `/`, que en una dirección de sed
# es el delimitador y parte la expresión.
instala=$(awk -v f="install_data('$FRAGMENTO'" \
    'index($0, f) { dentro = 1 } dentro { print } dentro && /\)/ { exit }' meson.build)
if [[ -n "$instala" ]]; then
    ok "y el paquete lo instala"
    if grep -qE "install_dir:.*'wireplumber'.*'wireplumber\.conf\.d'" <<<"$instala"; then
        ok "en el directorio donde WirePlumber los busca"
    else
        mal "no se instala en wireplumber.conf.d: WirePlumber no lo va a descubrir"
    fi
else
    mal "meson.build no instala $FRAGMENTO: el módulo no lo carga nadie"
fi

if grep -qE '^\s*name = libwireplumber-module-vasak-permisos-de-medios, type = module' "$FRAGMENTO"; then
    ok "declara el componente con el nombre del módulo"
else
    mal "el componente no se declara con el nombre que meson le pone al módulo"
fi

# Lo que decide si está encendido o no.
perfil=$(sed -n '/wireplumber.profiles/,/^}/p' "$FRAGMENTO")
if grep -qE 'custom\.vasak-permisos-de-medios\s*=\s*required' <<<"$perfil"; then
    ok "y viene encendido en el perfil"
elif grep -qE 'custom\.vasak-permisos-de-medios\s*=\s*(disabled|optional)' <<<"$perfil"; then
    mal "viene APAGADO u opcional: el permiso de cámara no se haría cumplir y la pantalla de Privacidad prometería de más"
else
    mal "el perfil no dice nada sobre el componente: no se sabe si carga"
fi

# Y que quede escrito cómo apagarlo. Un permiso que se hace cumplir sin salida
# de emergencia documentada se termina apagando a los golpes —desinstalando el
# paquete— y ahí se va también lo que sí servía.
if grep -qE 'custom\.vasak-permisos-de-medios\s*=\s*disabled' "$FRAGMENTO"; then
    ok "y el propio fragmento dice cómo apagarlo en un equipo"
else
    mal "el fragmento no documenta cómo apagarlo"
fi

# Y que el nombre del módulo coincida con el que compila meson. Son dos
# cadenas en dos archivos distintos y separarlas deja el fragmento apuntando a
# un módulo que no existe — WirePlumber arranca igual y no carga nada.
if grep -q "shared_module('wireplumber-module-vasak-permisos-de-medios'" src/permisos-de-medios/meson.build; then
    ok "y el nombre coincide con el que compila meson"
else
    mal "el módulo no se llama como el fragmento dice"
fi

printf '\n'
if [[ "$fallos" -eq 0 ]]; then
    printf '\033[32mSin fallos.\033[0m\n'
else
    printf '\033[31m%s fallo(s).\033[0m\n' "$fallos"
fi
exit $(( fallos > 0 ? 1 : 0 ))
