#!/usr/bin/env bash
#
# Que el módulo viaje en el paquete **apagado**, y que el fragmento que lo
# declara se instale.
#
# Las dos mitades importan y ninguna sola alcanza:
#
#   · Un fragmento que no se instala deja el módulo en el disco y sin cargar
#     nunca, y nada lo dice. A este taller ya le pasó con un drop-in de GRUB
#     que estuvo escrito y sin empaquetar durante meses.
#   · Un fragmento que se instala **encendido** apaga la cámara de todo el
#     equipo, porque una aplicación que nunca preguntó no tiene decisión
#     guardada y el módulo niega. Y como no figura en Privacidad y seguridad,
#     no hay interruptor que mover ni aviso que lo ofrezca: sería bloquear sin
#     poder desbloquear, que es lo que el escritorio no hace.
#
# Cuando eso se arregle —aviso en la negación, o la pantalla listando a las que
# no preguntaron— esta prueba se da vuelta a propósito, en el mismo commit que
# lo encienda.
#
# Uso: pruebas/viene-apagado.sh
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

FRAGMENTO=datos/50-vasak-permisos-de-medios.conf
fallos=0
ok()  { printf '  \033[32m✓\033[0m %s\n' "$1"; }
mal() { printf '  \033[31m✗\033[0m %s\n' "$1"; fallos=$((fallos + 1)); }

if [ -f "$FRAGMENTO" ]; then
    ok "el fragmento existe"
else
    mal "falta $FRAGMENTO"
fi

if grep -qE "install_data\('$FRAGMENTO'" meson.build; then
    ok "y el paquete lo instala"
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
if grep -qE 'custom\.vasak-permisos-de-medios\s*=\s*disabled' <<<"$perfil"; then
    ok "y viene apagado en el perfil"
elif grep -qE 'custom\.vasak-permisos-de-medios\s*=\s*(required|optional)' <<<"$perfil"; then
    mal "viene ENCENDIDO: apagaría la cámara del equipo sin forma de volver a encenderla"
else
    mal "el perfil no dice nada sobre el componente: no se sabe si carga"
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
if [ "$fallos" -eq 0 ]; then
    printf '\033[32mSin fallos.\033[0m\n'
else
    printf '\033[31m%s fallo(s).\033[0m\n' "$fallos"
fi
exit $(( fallos > 0 ? 1 : 0 ))
