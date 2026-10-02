#!/usr/bin/env bash
#
# Cuánto cuesta el ecualizador: latencia y procesador, con números.
#
# No es una prueba —no la corre `meson test`, y no falla—: es la medición que
# se cita en el README y en el PR, para que se pueda repetir. Corre en la pila
# aislada de `isolated-session.sh`, nunca en la de la sesión.
#
#   · **Latencia agregada**, medida y no declarada: un impulso grabado a la vez
#     en la entrada del filtro (el monitor de `vasak-equalizer`) y en la salida
#     (el monitor de `test-sink-a`), con la misma grabación, así que las dos
#     muestras salen del mismo ciclo del grafo. La diferencia entre los dos
#     picos es lo que tarda el audio en atravesar el filtro.
#   · **Procesador**: el tiempo de CPU del demonio de PipeWire durante
#     MEASURE_SECONDS de reproducción, con el ecualizador en «rock» y con el
#     ecualizador apagado. Antes de medir se esperan SETTLE_SECONDS, porque
#     comparar apenas arrancado mide el arranque y no el estado estable.
#   · **El nodo en el grafo**: lo que dice `pw-top` del filtro —tiempo de
#     proceso por ciclo— en esa misma corrida.
#
# Uso: ISOLATED_MODULE=… tests/equalizer/measure.sh [QUANTUM]
set -uo pipefail

if [[ -z "${VASAK_EQ_PRIVATE_BUS:-}" ]]; then
    export VASAK_EQ_PRIVATE_BUS=1
    exec dbus-run-session -- "$0" "$@"
fi

HERE=$(cd "$(dirname "$0")" && pwd)
: "${ISOLATED_MODULE:?hace falta la ruta del módulo compilado}"
QUANTUM=${1:-1024}
SETTLE_SECONDS=${SETTLE_SECONDS:-30}
MEASURE_SECONDS=${MEASURE_SECONDS:-30}
# shellcheck source=isolated-session.sh
source "$HERE/isolated-session.sh"

WORK=$(mktemp -d)
trap 'kill "${PLAYER:-}" 2>/dev/null; isolated_stop; rm -rf "$WORK"' EXIT

eq() {
    local method=$1
    shift
    gdbus call --session -d org.vasak.Equalizer -o /org/vasak/Equalizer \
        -m "org.vasak.Equalizer1.$method" "$@" >/dev/null
    sleep 0.5
    return 0
}

cpu_ticks() {
    local pid=$1
    # utime + stime, campos 14 y 15, después del último «)».
    sed 's/.*) //' "/proc/$pid/stat" | awk '{ print $12 + $13 }'
    return 0
}

echo "carga de la máquina al empezar: $(cut -d' ' -f1-3 /proc/loadavg) ($(nproc) núcleos)"
isolated_start || { echo "la pila aislada no arrancó; registros en $ISOLATED_ROOT"; export ISOLATED_KEEP=1; exit 1; }
pw-metadata -n settings 0 clock.force-quantum "$QUANTUM" >/dev/null
for _ in $(seq 200); do
    gdbus call --session -d org.vasak.Equalizer -o /org/vasak/Equalizer \
        -m org.freedesktop.DBus.Properties.Get org.vasak.Equalizer1 Available 2>/dev/null \
        | grep -q true && break
    sleep 0.05
done

echo "pipewire $(pipewire --version | sed -n 's/.*libpipewire //p' | head -n1), quantum $QUANTUM a 48 kHz ($(python3 -c "print(f'{$QUANTUM/48:.2f}')") ms)"

# ── Latencia ──────────────────────────────────────────────────────────────
python3 - "$WORK/impulse.raw" <<'PY'
import array, sys
d = array.array("f", [0.0] * (48000 * 2 * 2))
for k in range(4):                     # un impulso cada medio segundo
    i = (12000 + k * 24000) * 2
    d[i] = d[i + 1] = 0.5
d.tofile(open(sys.argv[1], "wb"))
PY
pw-record -a --format f32 --rate 48000 --channels 2 \
    -P '{ node.autoconnect = false  node.name = test-recorder }' "$WORK/both.raw" >/dev/null 2>&1 &
REC=$!
for _ in $(seq 100); do
    pw-link vasak-equalizer:monitor_FL test-recorder:input_FL 2>/dev/null && break
    sleep 0.02
done
pw-link test-sink-a:monitor_FL test-recorder:input_FR
pw-cat -p -a --format f32 --rate 48000 --channels 2 \
    -P '{ node.name = test-player }' "$WORK/impulse.raw" >/dev/null 2>&1
sleep 0.2
kill "$REC"; wait "$REC" 2>/dev/null
python3 - "$WORK/both.raw" <<'PY'
import array, sys
d = array.array("f"); d.frombytes(open(sys.argv[1], "rb").read())
before, after = d[0::2], d[1::2]
peaks = lambda ch: [i for i in range(1, len(ch)) if ch[i] > 0.25 and ch[i - 1] <= 0.25]
a, b = peaks(before), peaks(after)
if not a or len(a) != len(b):
    print(f"latencia: no se encontraron los impulsos ({len(a)} y {len(b)})")
else:
    diffs = sorted({y - x for x, y in zip(a, b)})
    print(f"latencia agregada por el filtro: {diffs} muestras "
          f"({', '.join(f'{v / 48:.3f}' for v in diffs)} ms), en {len(a)} impulsos")
PY

# ── Procesador ────────────────────────────────────────────────────────────
python3 "$HERE/tone.py" gen 440 $((SETTLE_SECONDS * 2 + MEASURE_SECONDS * 2 + 30)) "$WORK/long.raw"
pw-cat -p -a --format f32 --rate 48000 --channels 2 \
    -P '{ node.name = test-player }' "$WORK/long.raw" >/dev/null 2>&1 &
PLAYER=$!

# Los xruns del reproductor (la columna ERR de pw-top), que es lo que se oye
# como un chasquido. Es un contador acumulado: se mira cuánto sube dentro de
# la ventana de medición, no cuánto vale.
player_errors() {
    pw-top -b -n 2 2>/dev/null | awk '$NF == "test-player" { e = $9 } END { print e + 0 }'
    return 0
}

measure_cpu() {
    local label=$1 t0 t1 e0 e1
    sleep "$SETTLE_SECONDS"
    e0=$(player_errors)
    t0=$(cpu_ticks "$ISOLATED_PW_PID")
    if [[ $label == encendido ]]; then
        pw-top -b -n 3 > "$WORK/top.txt" 2>/dev/null &
    fi
    sleep "$MEASURE_SECONDS"
    t1=$(cpu_ticks "$ISOLATED_PW_PID")
    e1=$(player_errors)
    python3 -c "print(f'pipewire con el ecualizador $label: {($t1 - $t0) / $(getconf CLK_TCK) / $MEASURE_SECONDS * 100:.2f} % de un núcleo, {$e1 - $e0} xruns del reproductor en {$MEASURE_SECONDS} s')"
    return 0
}

eq SetPreset rock
measure_cpu encendido
eq SetEnabled false
measure_cpu apagado
eq SetEnabled true

echo "pw-top, el filtro y la salida (BUSY = tiempo de proceso por ciclo):"
grep -E 'S +ID|vasak-equalizer|test-sink-a|test-player' "$WORK/top.txt" | tail -n 5 | sed 's/^/    /'
