/*
 * Laermtracker - hardwareunabhaengiger Kern (reines C99)
 *
 * Eingabe : 24-bit-Samples (vorzeichenbehaftet, in int32) eines ICS-43434.
 * Ausgabe : je Ereignis ein Ordner/Dateisatz  /YYMMDD/HHMMSS.WAV|.SVG
 *           + Zeile in /EVENTS.CSV
 * Alle Dateizugriffe und die Uhr laufen ueber nc_io_t, damit der Kern auf dem
 * PC getestet werden kann (siehe tests/).
 */
#ifndef NOISE_CORE_H
#define NOISE_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Kalibrierung ICS-43434: 94 dB SPL @1 kHz = -26 dBFS (Spitzenwert-Konvention) ---- */
#define NC_FULLSCALE_24BIT   8388608.0          /* 2^23 */
#define NC_MIC_SENS_DBFS     (-26.0)
#define NC_REF_SPL_DB        94.0

typedef struct {
    uint32_t fs;               /* Abtastrate in Hz (Vielfaches von 100)            */
    float    cal_offset_db;    /* Feinkalibrierung, wird zum Pegel addiert         */
    float    trig_delta_db;    /* Ausloesung: Pegel > Hintergrund + delta          */
    float    trig_min_dba;     /* ... und Pegel >= dieser Absolutwert (dB(A))      */
    float    end_delta_db;     /* Ende: Pegel < Hintergrund + delta                */
    float    end_hyst_db;      /* ... bzw. < trig_min_dba - hyst (der groessere)   */
    uint32_t pre_ms;           /* Vorlauf (wird auf 100 ms gerundet)               */
    uint32_t post_ms;          /* Nachlauf nach letztem Geraeusch                  */
    uint32_t max_event_ms;     /* harte Obergrenze je Ereignis                     */
    float    slam_lafmax_dba;  /* LAFmax >= Wert  => Klasse KNALL                  */
    float    impulse_rise_db;  /* Anstieg >= Wert in 10 ms => impulshaft (TUER)    */
} nc_config_t;

typedef struct {
    void   *user;
    int64_t (*now_ms)(void *user);                       /* Unix-Zeit in ms (lokal/UTC egal) */
    int     (*mkdir)(void *user, const char *path);      /* 0 = ok oder existiert schon      */
    void   *(*open)(void *user, const char *path, char mode); /* mode 'w' neu, 'a' anhaengen */
    int     (*write)(void *user, void *h, const void *buf, uint32_t n); /* 0 = ok            */
    int     (*seek)(void *user, void *h, uint32_t pos);
    int     (*close)(void *user, void *h);
} nc_io_t;

#define NC_MAX_PLOT_BINS   720          /* 100-ms-Bins => 72 s */
#define NC_CHUNK_SAMPLES   1024
#define NC_HIST_FRAMES     256

#define NC_FQ_LEN          128

typedef struct { float meansq; float peak; uint32_t pos; } nc_frame_t;

typedef enum { NC_IDLE = 0, NC_ACTIVE = 1 } nc_state_t;

typedef struct {
    float b[3], a[3];
    float z1, z2;
} nc_biquad_t;

typedef struct {
    nc_config_t cfg;
    nc_io_t     io;

    /* --- ISR-Seite (nc_push24) --- */
    int16_t    *ring;               /* vom Aufrufer bereitgestellt, 16 bit */
    uint32_t    ring_len;
    uint32_t    M;                  /* Positionsmodul (Vielfaches von ring_len) */
    uint32_t    widx;               /* Schreibindex im Ring */
    volatile uint32_t wr;           /* Schreibposition [0,M) */
    nc_biquad_t aw[3];              /* A-Bewertung */
    uint32_t    frame_len;          /* Samples je 10-ms-Frame */
    uint32_t    frame_pos;
    float       frame_sumA;         /* Summe (A-bewertet)^2 */
    float       frame_peak;
    nc_frame_t  fq[NC_FQ_LEN];      /* Frame-Warteschlange ISR -> Hauptschleife */
    volatile uint32_t fq_head, fq_tail;
    volatile uint32_t fq_dropped;

    /* --- Hauptschleife (nc_poll) --- */
    uint32_t    rd;                 /* naechste noch nicht gespeicherte Position */
    uint32_t    overruns;           /* verlorene Samples (SD zu langsam) */
    float       ref_rms2;           /* Referenz-Effektivwert^2 fuer 94 dB */
    float       ref_peak;

    float       bg_db;              /* Hintergrundpegel */
    uint32_t    frames_seen;
    float       laf_energy;         /* Fast-Bewertung (125 ms) */
    float       prev_db;
    uint16_t    hist[NC_HIST_FRAMES]; /* LAF je Frame, 0.1 dB, fuer Vorlauf-Plot */
    uint32_t    hist_n;

    nc_state_t  state;
    /* laufendes Ereignis */
    int64_t     ev_start_ms;        /* Zeit des ersten Samples in der Datei */
    uint32_t    ev_pre_frames;
    uint32_t    ev_frames;          /* Frames seit Dateianfang */
    uint32_t    ev_last_active;     /* Frame-Index der letzten Aktivitaet */
    uint32_t    ev_trig_frame;
    uint32_t    ev_max_frames;
    uint32_t    ev_post_frames;
    double      ev_sumA;            /* laufende Energie seit Ausloesung */
    uint32_t    ev_n;               /* laufende Frame-Zahl seit Ausloesung */
    double      ev_sumA_snap;       /* Stand beim letzten aktiven Frame */
    uint32_t    ev_n_snap;
    float       ev_bg;              /* Hintergrundpegel bei Ausloesung */
    float       ev_lafmax, ev_lpeak, ev_rise;
    uint32_t    ev_lafmax_frame;
    int16_t     plot[NC_MAX_PLOT_BINS]; /* 0.1 dB, Max je 100 ms */
    uint32_t    plot_n;
    int16_t     bin_max;
    uint32_t    bin_cnt;
    char        dir[8], base[16];
    void       *wav;
    uint32_t    wav_data_bytes;
    uint32_t    events_total;
    uint32_t    holdoff;            /* Frames Pause nach SD-Fehler */

    int16_t     chunk[NC_CHUNK_SAMPLES];
} nc_t;

void nc_default_config(nc_config_t *c);
void nc_init(nc_t *n, const nc_config_t *c, const nc_io_t *io, int16_t *ring, uint32_t ring_len);
/* ISR-/DMA-Seite: 24-bit-Samples (int32, vorzeichenbehaftet) einspeisen. Kein Dateizugriff, kurz. */
void nc_push24(nc_t *n, const int32_t *s24, uint32_t count);
/* Hauptschleife: wartende Frames auswerten, Ereignisse erkennen und auf SD schreiben (darf blockieren) */
void nc_poll(nc_t *n);
/* Bequemlichkeit fuer Tests: push24 + poll */
void nc_process(nc_t *n, const int32_t *s24, uint32_t count);
/* laufendes Ereignis sauber abschliessen (z.B. vor Abschalten) */
void nc_flush(nc_t *n);

/* Hilfen (auch fuer Tests) */
float nc_a_weight_response_db(float f, float fs);   /* Betrag des digitalen Filters inkl. Normierung */
int64_t nc_ms_from_civil(int y, int mo, int d, int h, int mi, int s, int msec);
void  nc_civil_from_ms(int64_t ms, int *y, int *mo, int *d, int *h, int *mi, int *s, int *msec);

#ifdef __cplusplus
}
#endif
#endif
