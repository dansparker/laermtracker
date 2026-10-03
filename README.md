# Lärmtracker

[![CI](https://github.com/dansparker/laermtracker/actions/workflows/ci.yml/badge.svg)](https://github.com/dansparker/laermtracker/actions/workflows/ci.yml)

Lärmereignisse (Türen auf/zu, Türknallen) in der Wohnung erkennen und mit
**STM32F446 + ICS-43434 (I2S-MEMS-Mikrofon) + microSD** dokumentieren. Reines C (kein C++).

## Status
| Teil | Stand |
|---|---|
| Kern `core/` (Pegel, Erkennung, WAV/SVG/CSV) | per CI getestet gegen unabhängige Referenzberechnung (`tests/`) |
| Firmware `fw/` | baut in der CI (49 kB Flash, 115 kB RAM-Belegung inkl. Stack); **noch nicht auf Hardware ausprobiert** |

Die Hardware-Schicht (I2S-DMA-Format, SDIO, RTC) ist nach Datenblatt/HAL geschrieben, aber
ungetestet auf dem echten Board. Rückmeldungen/Issues sind willkommen.

## Funktionsweise
- Dauerhaftes Mithören mit 32 kHz / 24 bit (I2S-DMA); gespeichert werden nur Ereignisse.
- Ereignis = A-bewerteter 10-ms-Pegel > Hintergrund + 12 dB **und** ≥ 50 dB(A).
- Aufgezeichnet: 1 s Vorlauf bis 3 s nach dem letzten Geräusch (max. 60 s). Folgen mehrere
  Geräusche innerhalb der Nachlaufzeit, bleibt es ein Ereignis.
- Pro Ereignis (8.3-Namen, Ordner je Tag):
  - `/YYMMDD/HHMMSS.WAV` – 16 bit mono, **Zeitmarken**: BWF-`bext` (Datum, Uhrzeit, TimeReference)
    plus Cue-Marker „Auslösung“, „LAFmax“, „Ende Aktivität“ (in Audacity/Reaper sichtbar)
  - `/YYMMDD/HHMMSS.SVG` – Pegel-Zeit-Plot (LAF, 125 ms; Max je 100 ms) mit LAeq-Linie, LAFmax-Marke
  - `/EVENTS.CSV` – `datum;zeit;typ;aktiv_s;LAeq_dBA;LAFmax_dBA;Lpeak_dB;LAE_dBA;hintergrund_dBA;datei;ueberlaeufe`
- Einstufung (Heuristik, Schwellen in `fw/Inc/config.h`):
  `KNALL` (LAFmax ≥ 75 dB(A)), `TUER` (impulshafter Anstieg ≥ 10 dB innerhalb 10 ms), sonst `SONST`
  (z. B. Stimmen, Staubsauger). Es wird keine Geräuschquelle „erkannt“, nur Impulshaltigkeit und Pegel.

## Pegel / „objektivierbare“ Dezibelwerte
- ICS-43434: 94 dB SPL @ 1 kHz = −26 dBFS → daraus Schalldruckpegel; A-Bewertung als IIR-Filter
  (Abweichung zur IEC-Kurve ≤ 0,2 dB bis 2 kHz, ca. 0,6 dB bei 4 kHz, ca. 1,8 dB bei 8 kHz).
- Angegeben werden **LAeq** über die aktive Dauer, **LAFmax** (Fast, 125 ms), **Lpeak** (unbewertet,
  Spitzenwert), **LAE** (Einzelereignispegel) und der Hintergrundpegel.
- Das ist ein MEMS-Mikrofon ohne Eichung: sinnvoll für Vergleich und Dokumentation, aber **kein**
  Ersatz für ein geeichtes Schallpegelmessgerät (Klasse 1/2). Mit einem 94-dB-Kalibrator
  `CAL_OFFSET_DB` justieren. Das Gehäuse/der Einbauort verändern den Pegel.

## Hardware (WeAct STM32F446 CoreBoard V1.1, microSD ist auf dem Board)
| Mikrofon ICS-43434 | Board |
|---|---|
| 3V | 3V3 |
| GND | GND |
| SEL | GND (linker Kanal) |
| LRCL (WS) | PB12 |
| BCLK | PB13 |
| DOUT | PB15 |

SD-Karte: SDIO 4-Bit (PC8–PC12, PD2, auf dem Board verdrahtet), FAT32. Status-LED PB2:
an = Ereignis wird aufgezeichnet, 3× Blinken = bereit, schnelles Blinken = Fehler (SD/Takt).

## Uhrzeit
Die RTC läuft mit dem 32,768-kHz-Quarz (Batterie an VBAT hält sie bei Stromausfall). Zum Stellen
eine Datei `TIME.TXT` auf die SD-Karte legen, Inhalt z. B. `2026-10-03 14:30:00` (lokale Zeit); sie wird
beim Start gelesen und in `TIME.OLD` umbenannt.

## Bauen
```sh
# Kern + Tests auf dem PC (gcc)
make test && python3 tests/check_outputs.py out

# Firmware (arm-none-eabi-gcc)
sh fw/fetch_deps.sh      # lädt HAL, CMSIS, FatFs nach fw/deps
make -C fw               # -> fw/build/laermtracker.bin  (HSE nicht 8 MHz: make -C fw HSE_VALUE=25000000U)
```
Die CI baut beides bei jedem Push; die `.bin` und die Testausgaben (WAV/SVG/CSV) hängen als Artefakte an.
Flashen z. B. mit `st-flash write fw/build/laermtracker.bin 0x08000000` oder STM32CubeProgrammer (SWD-Stiftleiste am Board).

## Aufbau
- `core/noise_core.[ch]` – hardwareunabhängig. `nc_push24()` läuft im DMA-Interrupt (A-Filter, 10-ms-Frames,
  Ringpuffer), `nc_poll()` in der Hauptschleife (Zustandsautomat, Dateizugriffe; darf auf die SD warten, ohne Samples zu verlieren).
- `fw/` – HAL-Initialisierung, I2S-DMA, SDIO-Diskio für FatFs, RTC.
- `tests/` – synthetische Wohnung (Hintergrund 33 dB(A), Tür, Knall mit Nachschlag, langsames Anschwellen,
  1-kHz-Kalibrierton) gegen Referenzpegel mit analytischer A-Kurve; `check_outputs.py` prüft WAV/SVG/CSV.

## Grenzen
- Speicherung 16 bit (obere 16 von 24 Bit), Pegelberechnung mit vollen 24 bit.
- RAM-Ringpuffer 1,5 s; SD-Karten mit sehr langen Schreibpausen (> 0,5 s) können Samples verlieren
  (Zähler `ueberlaeufe` in der CSV).
- Zeitstempel haben die Genauigkeit der RTC-Auslesung (~ms) plus Verarbeitungsverzögerung, die der Kern herausrechnet.

## Lizenz
MIT
