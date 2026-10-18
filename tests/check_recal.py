#!/usr/bin/env python3
"""Prueft --recalibrate: check_recal.py <ordner> <delta_db> (EVENTS_ALT.CSV = Zustand vor dem Lauf)."""
import os, sys
import xml.etree.ElementTree as ET
out, delta = sys.argv[1], float(sys.argv[2])
bad = 0
def check(ok, msg):
    global bad
    print(("  [OK ] " if ok else "  [FAIL] ") + msg); bad += not ok
def rows(p): return [r.split(";") for r in open(p, encoding="utf-8").read().strip().splitlines()]
alt, new = rows(os.path.join(out, "EVENTS_ALT.CSV")), rows(os.path.join(out, "EVENTS.CSV"))
check(len(alt) == len(new) and len(new) > 1, f"{len(new) - 1} Ereignisse, Kopfzeile erhalten: {alt[0] == new[0]}")
for a, n in zip(alt[1:], new[1:]):
    ok = all(abs(float(n[i]) - (float(a[i]) + delta)) < 0.11 for i in (4, 5, 6, 7, 8))
    over = float(n[5]) - float(n[9])
    check(ok and abs(float(n[10]) - over) < 0.11 and n[11] == ("1" if over >= 0 else "0") and n[12] == a[12],
          f"{n[1][:8]}: LAFmax {a[5]}->{n[5]}, Stoerung {a[11]}->{n[11]}")
    svg = os.path.join(out, n[12][1:-3].replace("/", os.sep) + "SVG")
    r = ET.parse(svg).getroot()
    txt = " ".join("".join(e.itertext()) for e in r.iter() if e.tag.endswith("text"))
    check(("LAFmax = %s dB(A)" % n[5]) in txt and (("STÖRUNG" in txt) == (n[11] == "1")), f"  Ereignisplot: LAFmax {n[5]}, Stoerung={n[11]}")
day = os.path.join(out, new[1][12].split("/")[1])
check(rows(os.path.join(day, "TAG.CSV")) == new, "TAG.CSV == EVENTS.CSV")
r = ET.parse(os.path.join(day, "TAG.SVG")).getroot()
txt = " ".join("".join(e.itertext()) for e in r.iter() if e.tag.endswith("text"))
nst = sum(1 for n in new[1:] if n[11] == "1")
check(("%d Störungen" % nst) in txt, f"Tagesdiagramm nennt {nst} Stoerungen")
check(os.path.exists(os.path.join(out, "KORREKTUR.LOG")), "KORREKTUR.LOG vorhanden")
print("FEHLER" if bad else "Korrektur-Pruefung bestanden"); sys.exit(1 if bad else 0)
