#!/usr/bin/env bash
#
# Lo que suena: un tono a través del ecualizador, grabado a la salida, contra
# lo que deberían dar los biquads.
#
# `follows-output.sh` mira que el filtro esté en el camino y que el nodo tenga
# los valores; esto mira que esos valores **se oigan**. Son cosas distintas: un
# nombre de control que el grafo no conoce queda guardado en `Props` igual, y
# el nodo lo muestra como si sirviera.
#
#   · Plano y apagado suenan igual: el filtro en cero es transparente.
#   · «bass» y «rock» dan, en 63 Hz, 1 kHz y 4 kHz, el nivel que calcula
#     `tone.py expect` con las fórmulas de los biquads de PipeWire, ±0,3 dB.
#
# La grabación se engancha a mano al monitor de la salida. Si se la dejara a
# WirePlumber, la política de filtros la pondría a grabar **la entrada** del
# ecualizador —el monitor de la salida por omisión pasa a ser el del filtro—, y
# se mediría lo que entra y no lo que sale.
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
ok()  { printf '  \033[32m✓\033[0m %s\n' "$1"; return 0; }
mal() { printf '  \033[31m✗\033[0m %s\n' "$1"; fallos=$((fallos + 1)); return 0; }

eq() {
    local method=$1
    shift
    gdbus call --session -d org.vasak.Equalizer -o /org/vasak/Equalizer \
        -m "org.vasak.Equalizer1.$method" "$@" >/dev/null
    # Que la política termine de reenganchar, si cambió el encendido.
    sleep 0.5
    return 0
}

TONES=$(mktemp -d)
trap 'isolated_stop; rm -rf "$TONES"' EXIT

# El nivel de un tono de FREQ Hz, grabado del monitor de test-sink-a.
measure() {
    local freq=$1
    local tone="$TONES/$freq.raw" rec="$TONES/rec.raw"
    [[ -f $tone ]] || python3 "$HERE/tone.py" gen "$freq" 1.5 "$tone"
    rm -f "$rec"
    pw-record -a --format f32 --rate 48000 --channels 2 \
        -P '{ node.autoconnect = false  node.name = test-recorder }' "$rec" >/dev/null 2>&1 &
    local recorder=$!
    for _ in $(seq 100); do
        pw-link test-sink-a:monitor_FL test-recorder:input_FL 2>/dev/null && break
        sleep 0.02
    done
    pw-link test-sink-a:monitor_FR test-recorder:input_FR 2>/dev/null
    pw-cat -p -a --format f32 --rate 48000 --channels 2 \
        -P '{ node.name = test-player }' "$tone" >/dev/null 2>&1
    sleep 0.2
    kill "$recorder"; wait "$recorder" 2>/dev/null
    python3 "$HERE/tone.py" level "$rec"
    return $?
}

# Que dos niveles en dB estén a no más de TOL uno de otro.
close() {
    local a=$1 b=$2 tolerance=$3
    python3 -c "import sys; sys.exit(0 if abs($a - ($b)) <= $tolerance else 1)"
    return $?
}

check() {
    local preset=$1 freq=$2; shift 2
    local got want
    got=$(measure "$freq")
    want=$(python3 "$HERE/tone.py" expect "$freq" "$@")
    if [[ -z $got ]]; then
        mal "$preset en $freq Hz: sin señal en la grabación"
    elif close "$got" "$want" 0.3; then
        ok "$preset en $freq Hz: $got dBFS (esperado $want)"
    else
        mal "$preset en $freq Hz: $got dBFS, y los biquads dan $want"
    fi
    return 0
}

isolated_start || { echo "la pila aislada no arrancó; registros en $ISOLATED_ROOT"; export ISOLATED_KEEP=1; exit 1; }
for _ in $(seq 200); do
    gdbus call --session -d org.vasak.Equalizer -o /org/vasak/Equalizer \
        -m org.freedesktop.DBus.Properties.Get org.vasak.Equalizer1 Available 2>/dev/null \
        | grep -q true && break
    sleep 0.05
done

echo "── plano es transparente"
flat=$(measure 1000)
eq SetEnabled false
off=$(measure 1000)
eq SetEnabled true
if [[ -z $flat || -z $off ]]; then
    mal "1 kHz: sin señal en la grabación (plano: «$flat», sin el filtro: «$off»)"
elif close "$flat" "$off" 0.01; then
    ok "1 kHz plano: $flat dBFS; sin el filtro: $off dBFS"
else
    mal "1 kHz plano da $flat dBFS y sin el filtro $off dBFS: el filtro en cero no es transparente"
fi

echo "── los perfiles suenan como dicen"
eq SetPreset bass
check bass 63   6 5 4 2 0 0 0 0 0 0
check bass 1000 6 5 4 2 0 0 0 0 0 0
eq SetPreset rock
check rock 63   5 4 2 -1 -2 -1 2 3 4 4
check rock 4000 5 4 2 -1 -2 -1 2 3 4 4

printf '\n'
if [[ "$fallos" -eq 0 ]]; then
    printf '\033[32mSin fallos.\033[0m\n'
else
    printf '\033[31m%s fallo(s).\033[0m Registros en %s\n' "$fallos" "$ISOLATED_ROOT"
    export ISOLATED_KEEP=1
fi
exit $(( fallos > 0 ? 1 : 0 ))
