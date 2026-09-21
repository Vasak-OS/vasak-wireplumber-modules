#!/usr/bin/env bash
#
# Que el enganche se declare antes de `client/find-default-access`.
#
# Esto no lo ve el compilador ni ninguna prueba unitaria, y cuando está mal el
# módulo **no niega nada y no se queja**.
#
# Qué pasa: los enganches de acceso se ordenan por nombre, y todos los que
# reparten acceso se declaran «antes de client/apply-access». Si el nuestro
# nombra sólo a `apply-access`, queda sin orden respecto de
# `client/find-default-access` — que reparte el gestor por omisión— y éste gana
# la carrera. Cuando el nuestro corre, ya hay un gestor puesto, se aparta como
# corresponde, y la cámara queda abierta.
#
# Medido: con la declaración incompleta, el registro decía «ya tiene quien
# decida por él» para todos los clientes y capturar funcionaba 5 de 5. Con
# `client/find-default-access` en la lista, 0 de 5.
#
# Uso: pruebas/el-orden-del-enganche.sh
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

FUENTE=src/permisos-de-medios/modulo.c
fallos=0
ok()  { printf '  \033[32m✓\033[0m %s\n' "$1"; }
mal() { printf '  \033[31m✗\033[0m %s\n' "$1"; fallos=$((fallos + 1)); }

bloque=$(sed -n '/static const gchar \*antes\[\]/,/NULL };/p' "$FUENTE")

if [ -z "$bloque" ]; then
    mal "no se encontró la lista de precedencia en $FUENTE"
else
    for hook in client/find-default-access client/apply-access; do
        if grep -q "\"$hook\"" <<<"$bloque"; then
            ok "el enganche se declara antes de $hook"
        else
            mal "falta $hook en la lista: el módulo no va a negar nada, y en silencio"
        fi
    done
fi

# Y después del de configuración, que es quien aplica las reglas del archivo.
# Si corriéramos antes, le pisaríamos su gestor a quien lo puso a mano.
despues=$(sed -n '/static const gchar \*despues\[\]/,/NULL };/p' "$FUENTE")
if grep -q '"client/find-config-access"' <<<"$despues"; then
    ok "y después de client/find-config-access"
else
    mal "no se declara después de client/find-config-access: le pisaría el gestor a la configuración"
fi

printf '\n'
if [ "$fallos" -eq 0 ]; then
    printf '\033[32mSin fallos.\033[0m\n'
else
    printf '\033[31m%s fallo(s).\033[0m\n' "$fallos"
fi
exit $(( fallos > 0 ? 1 : 0 ))
