# Lärmtracker

Lärmereignisse (Türen auf/zu, Türknallen) in der Wohnung erkennen und mit
STM32F446 + ICS-43434 (I2S-MEMS-Mikrofon) + microSD dokumentieren. Reines C.

**Status: in Entwicklung.** Der hardwareunabhängige Kern (`core/`) und der Host-Test
(`tests/`) sind geschrieben, aber noch nicht kompiliert/getestet. Die Firmware-Schicht
(`fw/`) folgt.

## Funktionsweise
- Dauerhaftes Mithören mit 32 kHz / 24 bit; nur Ereignisse werden gespeichert.
- Ereignis = A-bewerteter Pegel (10-ms-Frames) > Hintergrund + 12 dB und ≥ 50 dB(A).
- Aufgezeichnet wird 1 s Vorlauf bis 3 s nach dem letzten Geräusch (max. 60 s).
- Je Ereignis: `/YYMMDD/HHMMSS.WAV` (16 bit, BWF-Zeitstempel + Cue-Marker),
  `/YYMMDD/HHMMSS.SVG` (Pegel-Zeit-Plot) und eine Zeile in `/EVENTS.CSV`
  mit LAeq, LAFmax, Lpeak (unbewertet), LAE.
- Einstufung: `KNALL` (LAFmax ≥ 75 dB(A)), `TUER` (impulshafter Anstieg ≥ 10 dB in 10 ms), sonst `SONST`.
  Das ist eine Heuristik; Schwellen in `nc_config_t`.

## Pegel / Kalibrierung
ICS-43434: 94 dB SPL @ 1 kHz = −26 dBFS. Die Pegel sind dadurch ungefähr, aber kein
Ersatz für ein geeichtes Schallpegelmessgerät. Mit einem 94-dB-Kalibrator lässt sich
`cal_offset_db` nachjustieren.

## Hardware (WeAct STM32F446 CoreBoard V1.1)
| Mikrofon | Pin |
|---|---|
| 3V / GND | 3V3 / GND |
| SEL | GND (linker Kanal) |
| LRCL (WS) | PB12 (I2S2_WS) |
| BCLK | PB13 (I2S2_CK) |
| DOUT | PB15 (I2S2_SD) |

microSD auf dem Board: SDIO 4-Bit (PC8–PC12, PD2).

## Lizenz
MIT
