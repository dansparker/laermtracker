#!/usr/bin/env python3
"""Prueft die Ausgabe der PC-Version (WAV-Eingabe-Test)."""
import glob, os, sys, wave
import xml.etree.ElementTree as ET
out = sys.argv[1]
bad = 0
def check(ok, msg):
    global bad
    print(("  [OK ] " if ok else "  [FAIL] ") + msg); bad += not ok
rows = [r.split(";") for r in open(os.path.join(out, "EVENTS.CSV"), encoding="utf-8").read().strip().splitlines()]
head, ev = rows[0], rows[1:]
check(len(head) == 15 and len(ev) == 3, f"EVENTS.CSV: {len(ev)} Ereignisse, {len(head)} Spalten (erwartet 3 / 15)")
if len(ev) == 3:
    laf = [float(r[5]) for r in ev]; zr = [r[2] for r in ev]; st = [r[11] for r in ev]
    check("typ" not in head, "keine Spalte 'typ' mehr")
    check(50 <= laf[0] < 75 and laf[1] > 75 and laf[2] > 75, f"LAFmax {laf}")
    check(zr == ["TAG", "TAG", "NACHT"], f"Zeitraum {zr} (Uhr lief ab 21:59:30)")
    check(st[1] == "1" and st[2] == "1", f"Stoerung bei den lauten Ereignissen {st}")
    check([r[0] for r in ev] == ["2026-10-03"] * 3, "Datum aus --start uebernommen")
wavs = sorted(glob.glob(os.path.join(out, "*", "2[12]*.WAV")))
check(len(wavs) == 3, f"{len(wavs)} WAV-Dateien")
for p in wavs:
    w = wave.open(p); dur = w.getnframes() / w.getframerate()
    check((w.getnchannels(), w.getsampwidth(), w.getframerate()) == (1, 2, 48000) and 3.0 < dur < 9.0, f"{os.path.basename(p)}: 48 kHz mono 16 bit, {dur:.1f} s")
for p in wavs:
    ET.parse(p[:-3] + "SVG")
check(os.path.exists(os.path.join(out, "261003", "TAG.SVG")), "Tagesdiagramm TAG.SVG vorhanden")
print("FEHLER" if bad else "PC-Pruefung bestanden"); sys.exit(1 if bad else 0)
