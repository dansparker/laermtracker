#!/usr/bin/env python3
"""Erzeugt eine synthetische EVENTS.CSV ueber 14 Tage fuer den Test der Auswertung.
   make_test_csv.py <ordner> [komma|punkt] [alt]   (alt = Format mit Spalte 'typ')"""
import math, os, random, sys, datetime as dt
out, dez = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "komma")
alt = "alt" in sys.argv[3:]
random.seed(3)
os.makedirs(out, exist_ok=True)
fmt = lambda v: ("%.1f" % v).replace(".", ",") if dez == "komma" else "%.1f" % v
kopf = ["datum", "zeit", "zeitraum"] + (["typ"] if alt else []) + ["aktiv_s", "LAeq_dBA", "LAFmax_dBA", "Lpeak_dB", "LAE_dBA", "hintergrund_dBA",
        "grenzwert_dBA", "ueber_dB", "stoerung", "datei", "ueberlaeufe", "schreibfehler"]
rows = []
start = dt.datetime(2026, 10, 5)
for d in range(14):
    for slot in range(48):                                    # Ereignisse oft zur halben/vollen Stunde
        t = start + dt.timedelta(days=d, minutes=30 * slot + random.choice([0, 0, 1, 2]) )
        p = 0.45 if 7 <= t.hour < 22 else 0.12
        if random.random() > p:
            continue
        t += dt.timedelta(seconds=random.randint(0, 50), milliseconds=random.randint(0, 999))
        nacht = t.hour >= 22 or t.hour < 6
        laf = random.gauss(62 if not nacht else 55, 9)
        lim = 42.0 if nacht else 55.0
        akt = abs(random.gauss(0.5, 0.4)) + 0.02
        rows.append((t, nacht, laf, lim, akt))
with open(os.path.join(out, "EVENTS.CSV"), "w", encoding="utf-8") as f:
    f.write(";".join(kopf) + "\n")
    for t, nacht, laf, lim, akt in rows:
        lae = laf - 6 + 10 * math.log10(max(akt, 0.05)) + 6
        f.write(";".join([t.strftime("%Y-%m-%d"), t.strftime("%H:%M:%S.") + "%03d" % (t.microsecond // 1000), "NACHT" if nacht else "TAG"]
                         + (["KNALL" if laf > 75 else "TUER"] if alt else [])
                         + [fmt(akt).replace(",", ".") if dez == "punkt" else ("%.2f" % akt).replace(".", ","), fmt(laf - 3), fmt(laf), fmt(laf + 18), fmt(lae), fmt(30.0),
                            fmt(lim), fmt(laf - lim), "1" if laf >= lim else "0", "/%s/%s.WAV" % (t.strftime("%y%m%d"), t.strftime("%H%M%S")), "0", "0"]) + "\n")
print("%d Ereignisse nach %s" % (len(rows), out))
