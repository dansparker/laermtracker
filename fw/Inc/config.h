/* Anpassbare Einstellungen der Firmware */
#ifndef CONFIG_H
#define CONFIG_H

#define SAMPLE_RATE_HZ     32000u   /* PLLI2S N=256, R=5 (bei 1 MHz VCO-Eingang) ergibt exakt 32 kHz */
#define RING_SAMPLES       48000u   /* 1,5 s: 1 s Vorlauf + 0,5 s Reserve fuer SD-Wartezeiten (96 kB RAM) */
#define DMA_WORDS          1024u    /* 32-bit-Woerter (L+R) im DMA-Puffer => 16 ms; Halbpuffer = 8 ms */
#define MIC_CHANNEL        0u       /* 0 = links (SEL an GND), 1 = rechts (SEL an 3V3) */
#define SD_CLKDIV          2u       /* SDIO-Takt = 48 MHz / (div + 2) */

/* Pegel / Ereigniserkennung (Erklaerung in core/noise_core.h) */
#define CAL_OFFSET_DB      0.0f     /* Feinkalibrierung mit 94-dB-Kalibrator */
#define TRIG_DELTA_DB      12.0f
#define TRIG_MIN_DBA       50.0f
#define PRE_MS             1000u
#define POST_MS            3000u
#define SLAM_LAFMAX_DBA    75.0f

#endif
