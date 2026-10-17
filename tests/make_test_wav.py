#!/usr/bin/env python3
"""Erzeugt eine synthetische 48-kHz-Testaufnahme: Hintergrund + Tuer (10 s) + Knall (25 s) + Knall in der Nacht (50 s)."""
import array, math, random, sys, wave
FS, SECONDS = 48000, 60
random.seed(5)
x = array.array('h', [0]) * (FS * SECONDS)
for i in range(len(x)):
    x[i] = int(random.gauss(0, 3))
def burst(t0, dur, sigma, tau):
    i0 = int(t0 * FS)
    for k in range(int(dur * FS)):
        t = k / FS
        env = (1 - math.exp(-t / 0.002)) * math.exp(-t / tau)
        v = x[i0 + k] + int(random.gauss(0, sigma) * env)
        x[i0 + k] = max(-32768, min(32767, v))
burst(10, 0.8, 150, 0.05)      # Tuer ins Schloss
burst(25, 1.5, 2500, 0.10)     # Tuer zuknallen
burst(50, 1.5, 2500, 0.10)     # Knall nach 22:00 Uhr
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS); w.writeframes(x.tobytes()); w.close()
