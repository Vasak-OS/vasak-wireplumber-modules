#!/usr/bin/env bash
#
# De punta a punta, en una pila de audio aislada: que el ecualizador se ponga
# delante de la salida por omisión, que la siga cuando cambia, que apagarlo lo
# saque del camino, y que lo elegido vuelva después de reiniciar PipeWire y
# WirePlumber.
#
# Es la prueba que pedía el issue #10 —«cómo se prueba que el filtro sigue a la
# salida»— y la única que mira enlaces de verdad: lo demás se puede romper con
# todas las pruebas unitarias en verde, porque quien engancha es la política de
# WirePlumber y no este repositorio.
#
# Corre dentro de su propio `dbus-run-session`: el módulo toma
# `org.vasak.Equalizer` en el bus de la sesión, y no puede ser el de la persona.
# No toca la pila de audio de la sesión (ver `isolated-session.sh`).
#
# Uso: ISOLATED_MODULE=build/src/equalizer/libwireplumber-module-vasak-equalizer.so \
#      tests/equalizer/follows-output.sh
set -uo pipefail

if [[ -z "${VASAK_EQ_PRIVATE_BUS:-}" ]]; then
    export VASAK_EQ_PRIVATE_BUS=1
    exec dbus-run-session -- "$0" "$@"
fi

HERE=$(cd "$(dirname "$0")" && pwd)
: "${ISOLATED_MODULE:?hace falta la ruta del módulo compilado}"
# shellcheck source=isolated-session.sh
source "$HERE/isolated-session.sh"

fallos=0
fallidas=()
ok()  { printf '  \033[32m✓\033[0m %s\n' "$1"; return 0; }
mal() {
    printf '  \033[31m✗\033[0m %s\n' "$1"
    fallos=$((fallos + 1))
    fallidas+=("$1")
    return 0
}

eq() {
    local method=$1
    shift
    gdbus call --session -d org.vasak.Equalizer -o /org/vasak/Equalizer \
        -m "org.vasak.Equalizer1.$method" "$@" >/dev/null
    return $?
}
eq_get() {
    local property=$1
    gdbus call --session -d org.vasak.Equalizer -o /org/vasak/Equalizer \
        -m org.freedesktop.DBus.Properties.Get org.vasak.Equalizer1 "$property" 2>/dev/null
    return $?
}

# Espera hasta 5 s a que `links_by_name` muestre (o deje de mostrar) un enlace.
# La política engancha en su bucle, no en el momento de la llamada.
wait_link() {
    local want=$1 link=$2
    for _ in $(seq 100); do
        if links_by_name | grep -qxF "$link"; then
            [[ $want == yes ]] && return 0
        else
            [[ $want == no ]] && return 0
        fi
        sleep 0.05
    done
    return 1
}

check_link() {
    local want=$1 link=$2 label=$3
    if wait_link "$want" "$link"; then
        ok "$label"
    else
        mal "$label"
        links_by_name | sed 's/^/      /'
    fi
    return 0
}

# La ganancia de un control del filtro, leída del propio nodo.
node_gain() {
    local control=$1
    pw-dump 2>/dev/null | jq -r --arg c "$control" '
        .[] | select(.type == "PipeWire:Interface:Node"
                     and .info.props["node.name"] == "vasak-equalizer")
        | (.info.params.Props // [])[] | (.params // empty)
        | select(index($c) != null) | .[index($c) + 1] * 1' 2>/dev/null | head -n1
    return 0
}

# Espera hasta 3 s a que un control valga lo que se le pidió: `set_param` no
# espera respuesta, y el nodo lo toma en su vuelta.
check_gain() {
    local control=$1 want=$2 label=$3 got
    for _ in $(seq 60); do
        got=$(node_gain "$control")
        [[ $got == "$want" ]] && { ok "$label"; return 0; }
        sleep 0.05
    done
    mal "$label (vale ${got:-nada})"
    return 0
}

wait_available() {
    for _ in $(seq 200); do
        eq_get Available | grep -q true && return 0
        sleep 0.05
    done
    return 1
}

start_player() {
    pw-cat -p -a --format f32 --rate 48000 --channels 2 \
        -P '{ node.name = test-player }' /dev/zero >/dev/null 2>&1 &
    PLAYER=$!
    return 0
}

trap 'kill "${PLAYER:-}" 2>/dev/null; isolated_stop' EXIT

isolated_start || { echo "la pila aislada no arrancó; registros en $ISOLATED_ROOT"; export ISOLATED_KEEP=1; exit 1; }

echo "── el filtro aparece y el módulo lo encuentra"
if wait_available; then ok "org.vasak.Equalizer dice Available = true"; else mal "el módulo no encontró el filtro"; fi

echo "── delante de la salida por omisión"
start_player
check_link yes "test-player -> vasak-equalizer" "el reproductor suena en el ecualizador"
check_link yes "vasak-equalizer.output -> test-sink-a" "y el ecualizador en la salida por omisión (A)"
default=$(wpctl inspect @DEFAULT_AUDIO_SINK@ | sed -n 's/.*node.name = "\(.*\)"/\1/p')
if [[ $default == test-sink-a ]]; then ok "el ecualizador no se robó la salida por omisión"; else mal "la salida por omisión es $default"; fi

# Lo que habla la API de PulseAudio —casi todo: navegadores, reproductores—
# entra por `pipewire-pulse`, que es otro cliente. Que también pase por el
# filtro. Sin `pipewire-pulse` o `pacat` instalados se dice y se sigue.
if command -v pipewire-pulse >/dev/null && command -v pacat >/dev/null; then
    echo "── lo que entra por PulseAudio, también"
    pipewire-pulse >> "$ISOLATED_ROOT/pipewire-pulse.log" 2>&1 &
    PULSE=$!
    for _ in $(seq 100); do [[ -S "$ISOLATED_RUN/pulse/native" ]] && break; sleep 0.05; done
    PULSE_SERVER=unix:$ISOLATED_RUN/pulse/native \
        pacat --raw --client-name=pulse-player /dev/zero >/dev/null 2>&1 &
    PACAT=$!
    check_link yes "pulse-player -> vasak-equalizer" "un cliente de PulseAudio suena en el ecualizador"
    # Lo que dice el README, mirado: grabar el monitor de la salida por omisión
    # graba la entrada del ecualizador, no su salida. Si WirePlumber lo cambia,
    # esto falla y el README tiene que cambiar con él.
    if command -v parecord >/dev/null; then
        PULSE_SERVER=unix:$ISOLATED_RUN/pulse/native \
            parecord --raw --client-name=monitor-recorder -d @DEFAULT_MONITOR@ /dev/null >/dev/null 2>&1 &
        RECORDER=$!
        check_link yes "vasak-equalizer -> monitor-recorder" \
            "grabar el monitor de la salida graba la entrada del ecualizador (README)"
        kill "$RECORDER" 2>/dev/null
        wait "$RECORDER" 2>/dev/null
    fi
    kill "$PACAT" "$PULSE" 2>/dev/null
    wait "$PACAT" "$PULSE" 2>/dev/null
else
    echo "── (sin pipewire-pulse o pacat: no se prueba la entrada por PulseAudio)"
fi

echo "── sigue al cambiar de salida"
wpctl set-default "$(node_id test-sink-b)"
check_link yes "vasak-equalizer.output -> test-sink-b" "pasa a la salida nueva (B)"
check_link no  "vasak-equalizer.output -> test-sink-a" "y suelta la anterior"
check_link yes "test-player -> vasak-equalizer" "el reproductor sigue en el ecualizador"

echo "── las ganancias llegan al nodo"
eq SetPreset rock
check_gain eq_31:Gain 5 "eq_31 en +5 dB (rock)"
check_gain eq_500:Gain -2 "eq_500 en -2 dB"
check_gain preamp:Gain -5 "y el margen en -5 dB"
eq SetGain 0 1.5
check_gain eq_31:Gain 1.5 "mover una banda la mueve en el nodo"
eq_get Preset | grep -q "'custom'" && ok "y el perfil pasa a custom" || mal "el perfil es $(eq_get Preset)"

echo "── apagado, fuera del camino"
eq SetEnabled false
check_link yes "test-player -> test-sink-b" "el reproductor va directo a la salida"
check_link no  "test-player -> vasak-equalizer" "y ya no pasa por el ecualizador"
eq SetEnabled true
check_link yes "test-player -> vasak-equalizer" "encendido de nuevo, vuelve a pasar"
check_link no  "test-player -> test-sink-b" "y no queda un enlace directo de más"

echo "── lo elegido sobrevive al reinicio"
eq SetPreset jazz
eq SetEnabled false
# El guardado espera medio segundo al último cambio: se espera hasta 5 s a que
# el archivo diga las dos cosas.
STATE_FILE=$XDG_STATE_HOME/vasak/equalizer.ini
saved=no
for _ in $(seq 100); do
    if grep -q '^preset=jazz' "$STATE_FILE" 2>/dev/null &&
       grep -q '^enabled=false' "$STATE_FILE" 2>/dev/null; then
        saved=yes
        break
    fi
    sleep 0.05
done
if [[ $saved == yes ]]; then
    ok "se guardó en XDG_STATE_HOME/vasak/equalizer.ini, perfil y apagado"
else
    mal "no está guardado: $(cat "$STATE_FILE" 2>&1)"
fi
kill "$PLAYER" 2>/dev/null
isolated_restart || mal "la pila aislada no volvió a arrancar"
wait_available || mal "después de reiniciar, el módulo no encontró el filtro"
check_gain eq_31:Gain 3 "PipeWire y WirePlumber reiniciados: jazz otra vez en el nodo"
eq_get Enabled | grep -q false && ok "y sigue apagado" || mal "Enabled es $(eq_get Enabled)"
start_player
# La salida por omisión que haya elegido WirePlumber al volver: qué salida es
# no importa acá —eso lo guarda WirePlumber, no este módulo—, sino que el
# reproductor vaya directo a ella.
default=$(wpctl inspect @DEFAULT_AUDIO_SINK@ | sed -n 's/.*node.name = "\(.*\)"/\1/p')
check_link yes "test-player -> $default" "apagado también después de reiniciar: directo a la salida"
check_link no  "test-player -> vasak-equalizer" "sin pasar por el ecualizador"

printf '\n'
if [[ "$fallos" -eq 0 ]]; then
    printf '\033[32mSin fallos.\033[0m\n'
else
    # Lo que falló, otra vez y al final: en CI sólo se ven las últimas líneas.
    printf '\033[31m%s fallo(s):\033[0m\n' "$fallos"
    printf '  · %s\n' "${fallidas[@]}"
    echo "── enlaces ahora"
    links_by_name | sed 's/^/    /'
    echo "── el registro de WirePlumber, lo último"
    tail -n 15 "$ISOLATED_ROOT/wireplumber.log" | sed 's/^/    /'
    echo "── el de PipeWire"
    tail -n 15 "$ISOLATED_ROOT/pipewire.log" | sed 's/^/    /'
    printf 'Registros en %s\n' "$ISOLATED_ROOT"
    export ISOLATED_KEEP=1
fi
exit $(( fallos > 0 ? 1 : 0 ))
