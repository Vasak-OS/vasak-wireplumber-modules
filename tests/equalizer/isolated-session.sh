#!/usr/bin/env bash
#
# Una pila de audio **aislada** —PipeWire + WirePlumber— para probar el
# ecualizador sin tocar la de la sesión.
#
# Se usa con `source`, desde una prueba que ya corre dentro de
# `dbus-run-session` (el módulo toma un nombre en el bus de la sesión, y el de
# la persona no se toca). Deja definidas `isolated_start` e `isolated_stop`.
#
# ── Qué la aísla ────────────────────────────────────────────────────────────
#
# · Su propio `XDG_RUNTIME_DIR`: los sockets `pipewire-0*` nacen ahí, y ningún
#   cliente de la sesión los encuentra, ni los de acá encuentran los de ella.
# · Su propio `XDG_STATE_HOME` y `XDG_CONFIG_HOME`: WirePlumber guarda ahí la
#   salida por omisión elegida, y el módulo el estado del ecualizador. Sin esto
#   la prueba escribiría en `~/.local/state` de la persona.
# · `PIPEWIRE_CONFIG_DIR` y `WIREPLUMBER_CONFIG_DIR` propios, con una copia de
#   las configuraciones de fábrica: lo instalado en `/etc` y `/usr/share` —por
#   ejemplo este mismo paquete, ya instalado— no entra, y el filtro no se carga
#   dos veces.
# · Sin dispositivos reales: los monitores de ALSA, Bluetooth y cámara están
#   apagados, y las salidas son dos `support.null-audio-sink`, `test-sink-a` y
#   `test-sink-b`. Una prueba que abriera la placa de sonido se la pelearía a
#   la sesión.
# · Sin el módulo de permisos de medios: le pregunta a `vasak-permissions` por
#   el bus del **sistema**, que no es aislable, y cada consulta queda anotada y
#   puede avisar.
#
# Nunca reinicia ni detiene la pila de la sesión.
set -uo pipefail

ISOLATED_ROOT=${ISOLATED_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/vasak-eq.XXXXXX")}
ISOLATED_REPO=${ISOLATED_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
# Dónde está el `.so` compilado del módulo. Vacío: se prueba sólo el filtro.
ISOLATED_MODULE=${ISOLATED_MODULE:-}

isolated_start() {
    local root=$ISOLATED_ROOT
    # El directorio de los sockets va aparte y corto: la ruta de un socket de
    # Unix no puede pasar de 108 bytes, y `pipewire-0-manager` dentro de un
    # directorio temporal largo los pasa. PipeWire no arranca, y lo dice.
    ISOLATED_RUN=$(mktemp -d /tmp/veq.XXXXXX)
    mkdir -p "$root/state" "$root/config" "$root/data" \
             "$root/pipewire/pipewire.conf.d" \
             "$root/wireplumber/wireplumber.conf.d" "$root/modules"
    chmod 700 "$ISOLATED_RUN"

    export XDG_RUNTIME_DIR=$ISOLATED_RUN
    export PIPEWIRE_RUNTIME_DIR=$ISOLATED_RUN
    export XDG_STATE_HOME=$root/state
    export XDG_CONFIG_HOME=$root/config
    export XDG_DATA_HOME=$root/data
    export PIPEWIRE_CONFIG_DIR=$root/pipewire
    export WIREPLUMBER_CONFIG_DIR=$root/wireplumber
    # Que ninguna herramienta herede el socket de la sesión por otro lado.
    unset PIPEWIRE_REMOTE DISPLAY WAYLAND_DISPLAY
    export PIPEWIRE_DEBUG=${PIPEWIRE_DEBUG:-1}
    # Sin esto GIO levanta gvfs en el bus privado, y gvfs intenta montar en
    # `/run/user/<uid>/gvfs` de la sesión de verdad.
    export GIO_USE_VFS=local

    # Todas las de fábrica: las herramientas (`pw-dump`, `wpctl`, `pw-cat`)
    # leen `client.conf` del mismo directorio.
    cp /usr/share/pipewire/*.conf "$root/pipewire/"
    cp "$ISOLATED_REPO/data/60-vasak-equalizer.conf" "$root/pipewire/pipewire.conf.d/"
    cat > "$root/pipewire/pipewire.conf.d/10-test-sinks.conf" <<'EOF'
context.properties = { module.x11.bell = false  module.jackdbus-detect = false }
context.objects = [
  { factory = adapter
    args = { factory.name = support.null-audio-sink
             node.name = test-sink-a  node.description = "Test sink A"
             media.class = Audio/Sink  audio.position = [ FL FR ]
             object.linger = true  priority.session = 1100 } }
  { factory = adapter
    args = { factory.name = support.null-audio-sink
             node.name = test-sink-b  node.description = "Test sink B"
             media.class = Audio/Sink  audio.position = [ FL FR ]
             object.linger = true  priority.session = 1000 } }
]
EOF

    cp /usr/share/wireplumber/wireplumber.conf "$root/wireplumber/"
    cat > "$root/wireplumber/wireplumber.conf.d/10-isolated.conf" <<'EOF'
wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.alsa-midi = disabled
    monitor.bluez = disabled
    monitor.bluez-midi = disabled
    monitor.bluez.seat-monitoring = disabled
    monitor.v4l2 = disabled
    monitor.libcamera = disabled
    support.logind = disabled
    support.modem-manager = disabled
    support.reserve-device = disabled
    custom.vasak-permisos-de-medios = disabled
  }
}
EOF

    # `WIREPLUMBER_MODULE_DIR` es el directorio entero, no una ruta de búsqueda:
    # los de fábrica enlazados más el nuestro.
    ln -sf /usr/lib/wireplumber-0.5/*.so "$root/modules/"
    rm -f "$root/modules/libwireplumber-module-vasak-"*.so
    if [[ -n "$ISOLATED_MODULE" ]]; then
        ln -sf "$ISOLATED_MODULE" "$root/modules/"
        cp "$ISOLATED_REPO/data/50-vasak-equalizer.conf" \
           "$root/wireplumber/wireplumber.conf.d/"
    fi
    export WIREPLUMBER_MODULE_DIR=$root/modules

    isolated_launch
    return $?
}

# Arranca los dos demonios sobre lo que dejó armado `isolated_start`. Sale
# con 1 si alguno no llega a estar listo en 10 s, y lo dice: una prueba que
# siguiera igual fallaría más adelante por un motivo que no es el suyo.
isolated_launch() {
    local ready=no
    pipewire >> "$ISOLATED_ROOT/pipewire.log" 2>&1 &
    ISOLATED_PW_PID=$!
    for _ in $(seq 200); do
        [[ -S "$ISOLATED_RUN/pipewire-0" ]] && { ready=yes; break; }
        sleep 0.05
    done
    if [[ $ready == no ]]; then
        echo "PipeWire aislado no abrió su socket en 10 s" >&2
        return 1
    fi
    WIREPLUMBER_DEBUG=${WIREPLUMBER_DEBUG:-2} wireplumber >> "$ISOLATED_ROOT/wireplumber.log" 2>&1 &
    ISOLATED_WP_PID=$!
    # Que WirePlumber haya terminado de activarse: ve las dos salidas y eligió
    # una por omisión. No es estado estable —para medir hay que esperar más,
    # ver `measure.sh`— pero alcanza para mirar enlaces.
    ready=no
    for _ in $(seq 200); do
        if wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null | grep -q 'node.name = "test-sink'; then
            ready=yes
            break
        fi
        sleep 0.05
    done
    if [[ $ready == no ]]; then
        echo "WirePlumber aislado no eligió una salida por omisión en 10 s" >&2
        return 1
    fi
    return 0
}

# Los dos demonios abajo y arriba otra vez, con el mismo estado en disco: es lo
# que pasa al cerrar y abrir la sesión.
isolated_restart() {
    kill "$ISOLATED_WP_PID" "$ISOLATED_PW_PID" 2>/dev/null
    wait "$ISOLATED_WP_PID" "$ISOLATED_PW_PID" 2>/dev/null
    isolated_launch
    return $?
}

isolated_stop() {
    [[ -n "${ISOLATED_WP_PID:-}" ]] && kill "$ISOLATED_WP_PID" 2>/dev/null
    [[ -n "${ISOLATED_PW_PID:-}" ]] && kill "$ISOLATED_PW_PID" 2>/dev/null
    wait 2>/dev/null
    ISOLATED_WP_PID= ISOLATED_PW_PID=
    rm -rf "${ISOLATED_RUN:-}"
    [[ -z "${ISOLATED_KEEP:-}" ]] && rm -rf "$ISOLATED_ROOT"
    return 0
}

# El id de un nodo por su `node.name`, o vacío.
node_id() {
    local name=$1
    pw-dump 2>/dev/null | jq -r --arg n "$name" \
        '.[] | select(.type == "PipeWire:Interface:Node" and .info.props["node.name"] == $n) | .id' | head -n1
    return 0
}

# Los enlaces como «salida -> entrada», por nombre de nodo, sin repetir canal.
links_by_name() {
    pw-dump 2>/dev/null | jq -r '
        (map(select(.type == "PipeWire:Interface:Node")) | map({key: (.id|tostring), value: .info.props["node.name"]}) | from_entries) as $n
        | .[] | select(.type == "PipeWire:Interface:Link")
        | "\($n[.info["output-node-id"]|tostring]) -> \($n[.info["input-node-id"]|tostring])"' | sort -u
    return 0
}
