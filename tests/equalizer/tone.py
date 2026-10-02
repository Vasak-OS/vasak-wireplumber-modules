#!/usr/bin/env python3
"""Tonos de prueba y niveles, para medir el ecualizador por lo que suena.

    tone.py gen FREQ SEGUNDOS SALIDA.raw   un seno estéreo f32 a 48 kHz, -12 dBFS
    tone.py level ENTRADA.raw              el nivel del tono grabado, en dBFS
    tone.py expect FREQ G31 … G16k         lo que debería sonar: la respuesta
                                            de los diez biquads y el margen

Sin numpy, para que corra en un contenedor pelado. La respuesta esperada usa
las mismas fórmulas que los biquads de PipeWire (`spa/plugins/filter-graph/
biquad.c`, que vienen del Audio EQ Cookbook de R. Bristow-Johnson): si el
filtro aplica algo distinto de lo que se le pidió, el nivel medido se aparta
de éste.
"""

import array
import cmath
import math
import sys

RATE = 48000
AMPLITUDE = 0.25  # -12 dBFS: ni +12 dB en una banda lo saca de rango
BANDS = [31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
Q = 1.41


def gen(freq, seconds, path):
    n = int(RATE * seconds)
    data = array.array("f")
    for i in range(n):
        v = AMPLITUDE * math.sin(2 * math.pi * freq * i / RATE)
        data.append(v)
        data.append(v)
    with open(path, "wb") as f:
        data.tofile(f)


def level(path):
    data = array.array("f")
    with open(path, "rb") as f:
        data.frombytes(f.read())
    left = data[0::2]
    # Lo que sonó, sin el silencio de antes y de después, y sin los bordes,
    # donde los biquads todavía se están asentando.
    loud = [i for i, v in enumerate(left) if abs(v) > 1e-3]
    if len(loud) < RATE // 2:
        print("sin señal", file=sys.stderr)
        sys.exit(1)
    first, last = loud[0], loud[-1]
    span = last - first
    middle = left[first + span // 4:last - span // 4]
    rms = math.sqrt(sum(v * v for v in middle) / len(middle))
    print(f"{20 * math.log10(rms):.3f}")


def peaking(f0, gain, f):
    a = 10 ** (gain / 40)
    w0 = 2 * math.pi * f0 / RATE
    alpha = math.sin(w0) / (2 * Q)
    b = (1 + alpha * a, -2 * math.cos(w0), 1 - alpha * a)
    d = (1 + alpha / a, -2 * math.cos(w0), 1 - alpha / a)
    z = cmath.exp(-1j * 2 * math.pi * f / RATE)
    h = (b[0] + b[1] * z + b[2] * z * z) / (d[0] + d[1] * z + d[2] * z * z)
    return abs(h)


def expect(freq, gains):
    db = -max(0, max(gains))  # el margen, igual que vasak_eq_preamp()
    for f0, g in zip(BANDS, gains):
        db += 20 * math.log10(peaking(f0, g, freq))
    rms = AMPLITUDE / math.sqrt(2)
    print(f"{20 * math.log10(rms) + db:.3f}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "gen":
        gen(float(sys.argv[2]), float(sys.argv[3]), sys.argv[4])
    elif cmd == "level":
        level(sys.argv[2])
    elif cmd == "expect":
        expect(float(sys.argv[2]), [float(g) for g in sys.argv[3:13]])
    else:
        sys.exit(f"orden desconocida: {cmd}")
