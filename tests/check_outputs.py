#!/usr/bin/env python3
"""Prueft die vom Test erzeugten Dateien: WAV-Struktur (RIFF/bext/cue), SVG-Wohlgeformtheit, CSV."""
import struct, sys, glob, os, wave
import xml.etree.ElementTree as ET

out = sys.argv[1] if len(sys.argv) > 1 else "out"
bad = 0
def check(ok, msg):
    global bad
    print(("  [OK ] " if ok else "  [FAIL] ") + msg)
    bad += not ok

wavs = sorted(glob.glob(os.path.join(out, "*", "*.WAV")))
check(len(wavs) == 4, f"{len(wavs)} WAV-Dateien (erwartet 4)")
for p in wavs:
    d = open(p, "rb").read()
    riff = struct.unpack_from("<I", d, 4)[0]
    check(d[:4] == b"RIFF" and riff + 8 == len(d), f"{p}: RIFF-Groesse {riff}+8 == {len(d)}")
    chunks, pos = {}, 12
    while pos + 8 <= len(d):
        cid, sz = d[pos:pos + 4], struct.unpack_from("<I", d, pos + 4)[0]
        chunks[cid] = (pos + 8, sz); pos += 8 + sz + (sz & 1)
    check(pos == len(d), "Chunk-Kette endet exakt am Dateiende")
    w = wave.open(p)           # Standard-Reader muss die Datei akzeptieren
    check((w.getnchannels(), w.getsampwidth(), w.getframerate()) == (1, 2, 32000), "mono / 16 bit / 32 kHz")
    dur = w.getnframes() / 32000
    check(2.0 < dur < 12.0, f"Dauer {dur:.2f} s (Vorlauf + Ereignis + Nachlauf)")
    b = chunks[b"bext"][0]
    desc = d[b:b + 256].split(b"\0")[0].decode()
    date, tm = d[b + 320:b + 330].split(b"\0")[0].decode(), d[b + 330:b + 338].split(b"\0")[0].decode()
    check(date == "2026-10-03" and tm[:2] in ("21", "22"), f"bext: {date} {tm} | {desc}")
    c, cs = chunks[b"cue "]; n = struct.unpack_from("<I", d, c)[0]
    check(n == 3, f"{n} Cue-Marker")
    l, ls = chunks[b"LIST"]
    labels = [x.decode(errors="replace") for x in d[l + 4:l + ls].split(b"labl")[1:]]
    check(len(labels) == 3, "Marker-Texte: " + " | ".join(t[4:].strip("\0 ") for t in labels))
for p in sorted(glob.glob(os.path.join(out, "*", "2[12]*.SVG"))):
    r = ET.parse(p).getroot()
    pts = [e for e in r.iter() if e.tag.endswith("polyline")][0].get("points").split()
    check(len(pts) >= 20, f"{p}: gueltiges XML, {len(pts)} Plotpunkte")
rows = open(os.path.join(out, "EVENTS.CSV"), encoding="utf-8").read().strip().splitlines()
check(len(rows) == 5 and rows[0].startswith("datum;") and rows[0].count(";") == 15, "EVENTS.CSV: Kopf (15 Spalten) + 4 Zeilen")
check([r.split(";")[12] for r in rows[1:]] == ["0", "1", "1", "1"], "Stoerungsspalte: 0,1,1,1")
day = open(os.path.join(out, "261003", "TAG.CSV"), encoding="utf-8").read().strip().splitlines()
check(day == rows, "TAG.CSV == EVENTS.CSV (alle Ereignisse an einem Tag)")
r = ET.parse(os.path.join(out, "261003", "TAG.SVG")).getroot()
txt = " ".join("".join(e.itertext()) for e in r.iter() if e.tag.endswith("text"))
marks = [e for e in r.iter() if e.tag.split("}")[-1] in ("circle", "rect", "polygon") and e.get("fill") in ("#d32f2f", "#1769aa")]
red = [e for e in marks if e.get("fill") == "#d32f2f"]
check(len(marks) == 4 and len(red) == 3, f"Tagesplot: {len(marks)} Marker, davon {len(red)} rot (Stoerung)")
check("4 Ereignisse" in txt and "3 Störungen" in txt and "nachts 2" in txt, "Tagesplot-Titel: " + txt[:110])
ev = ET.parse(sorted(glob.glob(os.path.join(out, "*", "2[12]*.SVG")))[1]).getroot()
check("STÖRUNG" in " ".join("".join(e.itertext()) for e in ev.iter() if e.tag.endswith("text")), "Ereignisplot des Knalls ist als STOERUNG markiert")
print("FEHLER" if bad else "Dateipruefung bestanden"); sys.exit(1 if bad else 0)
