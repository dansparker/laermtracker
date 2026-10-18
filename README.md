# Lärmtracker

[![CI](https://github.com/dansparker/laermtracker/actions/workflows/ci.yml/badge.svg)](https://github.com/dansparker/laermtracker/actions/workflows/ci.yml)

Lärmereignisse (Türen auf/zu, Türknallen) in der Wohnung erkennen und mit
**STM32F446 + ICS-43434 (I2S-MEMS-Mikrofon) + microSD** dokumentieren. Reines C (kein C++).

## Status
| Teil | Stand |
|---|---|
| Kern `core/` (Pegel, Erkennung, WAV/SVG/CSV) | per CI getestet: Referenzpegel (`tests/test_core.c`), Grenzfälle (`tests/test_edge.c`: Fehlausl., bit-genaue Audiodaten, Stillstand/Überlauf, Mitternacht, SD-Fehler, 310 Ereignisse/Tag), AddressSanitizer/UBSan, `-Werror` |
| Firmware `fw/` | baut in der CI (54 kB Flash, 119 kB von 128 kB RAM inkl. Stack); **noch nicht auf Hardware ausprobiert** |

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
  - `/YYMMDD/TAG.SVG` – **Tagesdiagramm**: alle Ereignisse des Tages über der Uhrzeit (Höhe = LAFmax, Fuß =
    Hintergrundpegel, ein Punkt = ein Ereignis), Nachtzeit blau hinterlegt, Grenzwertlinien, Störungen rot;
    wird nach jedem Ereignis aus `/YYMMDD/TAG.CSV` neu erzeugt
  - `/EVENTS.CSV` (alle Tage) und `/YYMMDD/TAG.CSV` (ein Tag) –
    `datum;zeit;zeitraum;aktiv_s;LAeq_dBA;LAFmax_dBA;Lpeak_dB;LAE_dBA;hintergrund_dBA;grenzwert_dBA;ueber_dB;stoerung;datei;ueberlaeufe;schreibfehler`
    (`zeit` = Auslösezeitpunkt, `stoerung` = 1/0)
- Es wird **keine Geräuschart** bestimmt (Tür, Knall, Stimme …), nur Pegel und Zeit. Jedes Ereignis, das die Auslöseschwelle überschreitet,
  wird gespeichert; ob es eine Störung ist, entscheidet allein der LAFmax gegen den Grenzwert (siehe unten).
  Alte Dateien mit einer Spalte `typ` werden von GUI und Tagesdiagramm weiterhin gelesen.

## Was gilt als „Lärmstörung“?
Es gibt **keinen einzelnen gesetzlichen Amplitudenwert** für Türgeräusche in der Wohnung; Gerichte/Gutachter
beurteilen im Einzelfall (Ortsüblichkeit, Zeit, Grundgeräuschpegel). Als objektive Orientierung markiert das Projekt
ein Ereignis als **Störung**, wenn sein **LAFmax** (Innenraum, am Mikrofon) den zeitabhängigen Grenzwert erreicht:

| Zeit (lokal) | Grenzwert LAFmax | Herkunft |
|---|---|---|
| Nacht 22–6 Uhr | **42 dB(A)** | WHO Night Noise Guidelines 2009: Schwelle für Aufwachreaktionen innen (die WHO-Community-Noise-Leitlinie 1999 nennt 45 dB für Schlafzimmer) |
| Tag 6–22 Uhr | 55 dB(A) | **eigene Annahme** (kein Normwert), in `fw/Inc/config.h` anpassbar |

Hinweise: Die Werte sind Orientierung, kein Rechtsgutachten. In Österreich werden Immissionen üblicherweise nach
ÖAL-Richtlinie Nr. 3 / ÖNORM S 5004 und im Vergleich zum Grundgeräuschpegel beurteilt; die genauen
Beurteilungskriterien bitte direkt dort nachlesen. Weil ein Ereignis erst ab `TRIG_MIN_DBA` (50 dB(A)) aufgezeichnet
wird, werden nachts leisere Geräusche (42–50 dB(A)) nicht erfasst – dafür `TRIG_MIN_DBA` senken. Die Spalte
`ueber_dB` zeigt, wie weit der Grenzwert überschritten wurde; Hintergrund und LAeq/LAE stehen zum Vergleich daneben.

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

SD-Karte: SDIO 4-Bit (PC8–PC12, PD2, auf dem Board verdrahtet), FAT32. Status-LED PB2 (blau, high = an):
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

## PC-Version (Windows und Linux, USB-Mikrofon, Ausgabe auf die Festplatte)
Derselbe Kern (`core/`) mit einem USB-/Systemmikrofon als Eingang; die Dateien landen auf der Festplatte
(Standard: Benutzerordner `Laermtracker`, mit `--out` änderbar), Struktur wie auf der SD-Karte.

```sh
laermtracker --list                          # Aufnahmegeräte anzeigen
laermtracker --calibrate --device 1          # Empfindlichkeit mit 94-dB-Kalibrator (1 kHz) messen -> gibt --sens aus
laermtracker --device 1 --sens -32           # Überwachung starten (Strg+C beendet)
laermtracker --wav-in aufnahme.wav --start "2026-10-03 21:59:30" --out ausgabe   # vorhandene WAV analysieren
```
- **Windows-Oberfläche:** `laermtracker-gui.exe` (reines C/Win32, kein Installer): Mikrofon wählen, Empfindlichkeit eintragen oder per
  „Kalibrieren“ messen, Ausgabeordner wählen, Start. Live-Pegelverlauf der letzten 2 Minuten, Ereignisliste (Störungen rot),
  Tagesdiagramm und CSV per Knopfdruck. Einstellungen werden in `%APPDATA%\Laermtracker\settings.ini` gespeichert.
  Bauen: `windres pc/gui.rc -O coff -o gui_rc.o && gcc -mwindows -o laermtracker-gui.exe pc/gui.c core/noise_core.c gui_rc.o -lwinmm -lcomctl32 -lshell32 -lole32 -lgdi32 -luser32 -lm`.
- **Windows:** fertige `laermtracker.exe` als CI-Artefakt `laermtracker-windows` (kein Installer nötig), oder mit
  MinGW selbst bauen: `make -C pc windows`. Eingang über WinMM (`--device N`).
- **Linux:** `sudo apt install libasound2-dev && make -C pc`, Eingang über ALSA (`--device default` oder `plughw:1,0`).
- **Nachträglich korrigieren (ohne Kalibrator):** Handy-Schallpegelmesser neben das Mikrofon legen, gleichmäßiges Rauschen abspielen und den Unterschied
  im Feld „Korrektur [dB]“ eintragen. „Vorhandene Aufnahmen umrechnen …“ (oder `laermtracker --recalibrate dB --out ORDNER`) rechnet alle bisherigen
  Ergebnisse um: CSV, Ereignis- und Tagesdiagramme, Störungen werden neu bewertet. Das Original bleibt als `EVENTS_ALT.CSV`,
  jede Anwendung steht in `KORREKTUR.LOG`. Nur einmal je Korrektur anwenden.
- **Wichtig:** Ein USB-Mikrofon hat eine unbekannte Empfindlichkeit. Ohne `--calibrate`/`--sens` sind die dB-Werte nur Schätzungen.
  Automatische Verstärkung (AGC) und „Klangverbesserungen“ in den Systemeinstellungen abschalten, sonst sind die Pegel unbrauchbar.
- Alle Schwellen (`--trig-min`, `--trig-delta`, `--post-ms`, `--lim-night`, …) stehen in `laermtracker --help`.
- Getestet in der CI: Analyse einer synthetischen Aufnahme (Tür, Knall, Knall nachts) unter Windows und Linux. Das Mikrofon
  selbst (WinMM/ALSA) ist ungetestet auf echter Hardware.

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
