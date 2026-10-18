/*
 * Laermtracker fuer Windows: gleiche Erkennung wie die STM32-Firmware (core/noise_core.c), Eingang ist ein USB-Mikrofon
 * (WinMM waveIn) oder - zum Testen/Nachanalysieren - eine WAV-Datei.
 *
 *   laermtracker --list                         Aufnahmegeraete anzeigen
 *   laermtracker --calibrate --device 1         Mikrofonempfindlichkeit mit 94-dB-Kalibrator (1 kHz) bestimmen
 *   laermtracker --device 1 --sens -32 --out D:\laerm      Ueberwachung starten (Strg+C beendet)
 *   (Linux: --device NAME statt Nummer, z.B. default oder plughw:1,0; Aufnahme ueber ALSA, Build mit -DHAVE_ALSA -lasound)
 *   laermtracker --wav-in aufnahme.wav --start "2026-10-03 21:59:30" --out out   WAV-Datei analysieren
 *
 * Ausgabe wie auf der SD-Karte: <out>\YYMMDD\HHMMSS.WAV/.SVG, <out>\YYMMDD\TAG.CSV/.SVG, <out>\EVENTS.CSV
 */
#define _CRT_SECURE_NO_WARNINGS
#include "../core/noise_core.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define PSEP "\\"
#else
#define PSEP "/"
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#define MKDIR(p) mkdir(p, 0777)
#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
#endif
#endif

static char g_out[260] = "laermtracker-daten";
static volatile int g_stop;
static nc_t g_nc;
static uint64_t g_pushed;
static int g_sim;
static int64_t g_sim_start_ms;
static uint32_t g_fs = 48000;
static float g_cal_db;

/* ---------------------------------------------------------------- Zeit / Dateien (fuer den Kern) */
static int64_t io_now(void *u)
{
    (void)u;
    if (g_sim) return g_sim_start_ms + (int64_t)(g_pushed * 1000ull / g_fs);
#ifdef _WIN32
    SYSTEMTIME st; GetLocalTime(&st);
    return nc_ms_from_civil(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm t; time_t sec = tv.tv_sec; localtime_r(&sec, &t);
    return nc_ms_from_civil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (int)(tv.tv_usec / 1000));
#endif
}
static void full(char *o, size_t sz, const char *p) { snprintf(o, sz, "%s%s", g_out, p); }
static int io_mkdir(void *u, const char *p) { (void)u; char b[400]; full(b, sizeof b, p); MKDIR(b); return 0; }
static void *io_open(void *u, const char *p, char m)
{
    (void)u; char b[400]; full(b, sizeof b, p);
    return fopen(b, m == 'r' ? "rb" : (m == 'a' ? "ab" : "wb"));
}
static int io_write(void *u, void *h, const void *b, uint32_t n) { (void)u; return fwrite(b, 1, n, (FILE *)h) == n ? 0 : -1; }
static int io_seek(void *u, void *h, uint32_t p) { (void)u; return fseek((FILE *)h, (long)p, SEEK_SET); }
static int io_close(void *u, void *h) { (void)u; return fclose((FILE *)h); }
static int32_t io_read(void *u, void *h, void *b, uint32_t n) { (void)u; return (int32_t)fread(b, 1, n, (FILE *)h); }
static int io_remove(void *u, const char *p) { (void)u; char b[400]; full(b, sizeof b, p); return remove(b); }

/* ---------------------------------------------------------------- Argumente */
static int g_argc; static char **g_argv;
static const char *opt(const char *name)
{
    for (int i = 1; i + 1 < g_argc; i++) if (!strcmp(g_argv[i], name)) return g_argv[i + 1];
    return NULL;
}
static int flag(const char *name) { for (int i = 1; i < g_argc; i++) if (!strcmp(g_argv[i], name)) return 1; return 0; }
static double optd(const char *name, double def) { const char *v = opt(name); return v ? atof(v) : def; }

/* ---------------------------------------------------------------- Status */
static float level_laf(void)
{
    float e = g_nc.laf_energy; if (e < 1e-6f) e = 1e-6f;
    return 10.0f * log10f(e / g_nc.ref_rms2) + NC_REF_SPL_DB + g_cal_db;
}
static int g_status_ms = 1000, g_log_lines;

static float trig_thr(void)
{
    float t = g_nc.bg_db + g_nc.cfg.trig_delta_db;
    return t < g_nc.cfg.trig_min_dba ? g_nc.cfg.trig_min_dba : t;
}

static void hms(char *o, size_t sz)
{
    int y, mo, d, h, mi, s, ms; nc_civil_from_ms(io_now(NULL), &y, &mo, &d, &h, &mi, &s, &ms);
    snprintf(o, sz, "%02d:%02d:%02d", h, mi, s);
}

/* Felder der letzten Zeile von EVENTS.CSV */
static int last_csv_fields(char *buf, size_t sz, char *fld[], int max)
{
    char p[400]; full(p, sizeof p, "/EVENTS.CSV");
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); long from = len > (long)sz - 1 ? len - (long)sz + 1 : 0;
    fseek(f, from, SEEK_SET); size_t n = fread(buf, 1, sz - 1, f); fclose(f); buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    char *line = strrchr(buf, '\n'); line = line ? line + 1 : buf;
    int nf = 0; fld[nf++] = line;
    for (char *q = line; *q && nf < max; q++) if (*q == ';') { *q = 0; fld[nf++] = q + 1; }
    return nf;
}

/* Wird nach jedem Audioblock aufgerufen: meldet Ausloesung, gespeicherte/verworfene Ereignisse und (periodisch) den Pegel */
static void ui_tick(void)
{
    static uint32_t seen_t, seen_d; static int was_active; static uint64_t last_ms;
    const char *nl = g_log_lines ? "" : "\n";                               /* Statuszeile nicht ueberschreiben */
    char ts[16];
    int act = g_nc.state == NC_ACTIVE;
    if (act && !was_active) {
        hms(ts, sizeof ts);
        printf("%s[%s] AUSGELOEST: LAF %.1f dB(A), Schwelle %.1f dB(A)\n", nl, ts, level_laf(), trig_thr());
    }
    was_active = act;
    if (g_nc.events_total != seen_t) {
        seen_t = g_nc.events_total;
        char buf[1024]; char *f[16]; int nf = last_csv_fields(buf, sizeof buf, f, 16);
        if (nf >= 14)
            printf("%s[%.8s] Ereignis gespeichert: %s, LAFmax %s dB(A), LAeq %s dB(A), %s s aktiv%s -> %s\n", nl, f[1], f[3], f[6], f[5], f[4],
                   !strcmp(f[12], "1") ? "  ** STOERUNG (+" : "", f[13]);
        else printf("%s[Ereignis gespeichert] %s" PSEP "%s.WAV\n", nl, g_nc.dir, g_nc.base);
        if (nf >= 14 && !strcmp(f[12], "1")) printf("   ueber Grenzwert: +%s dB **\n", f[11]);
    }
    if (g_nc.events_dropped != seen_d) { seen_d = g_nc.events_dropped; printf("%s[Ereignis verworfen: nicht impulshaft]\n", nl); }
    if (g_status_ms > 0) {
        uint64_t ms = g_pushed * 1000ull / g_fs;
        if (ms - last_ms >= (uint64_t)g_status_ms) {
            last_ms = ms;
            if (g_log_lines) { hms(ts, sizeof ts); printf("[%s] LAF %.1f dB(A) | Hintergrund %.1f | Schwelle %.1f%s\n", ts, level_laf(), g_nc.bg_db, trig_thr(), act ? " | AUFNAHME" : ""); }
            else printf("\rLAF %5.1f dB(A)  Hintergrund %5.1f  Schwelle %5.1f  %s   ", level_laf(), g_nc.bg_db, trig_thr(), act ? "[AUFNAHME]" : "          ");
        }
    }
    fflush(stdout);
}

/* ---------------------------------------------------------------- WAV-Eingabe */
static int16_t *read_wav(const char *path, uint32_t *rate, uint32_t *count)
{
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "Datei nicht lesbar: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len); if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return NULL; }
    fclose(f);
    if (len < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { fprintf(stderr, "keine WAV-Datei\n"); free(d); return NULL; }
    uint32_t ch = 0, bits = 0, fmt = 0; long pos = 12; int16_t *out = NULL;
    while (pos + 8 <= len) {
        uint32_t sz = d[pos + 4] | d[pos + 5] << 8 | d[pos + 6] << 16 | (uint32_t)d[pos + 7] << 24;
        const uint8_t *c = d + pos + 8;
        if (!memcmp(d + pos, "fmt ", 4)) { fmt = c[0] | c[1] << 8; ch = c[2] | c[3] << 8; *rate = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24; bits = c[14] | c[15] << 8; }
        else if (!memcmp(d + pos, "data", 4)) {
            if ((fmt != 1 && fmt != 0xFFFE) || bits != 16 || ch < 1) { fprintf(stderr, "nur 16-bit-PCM-WAV unterstuetzt\n"); free(d); return NULL; }
            if ((long)sz > len - pos - 8) sz = (uint32_t)(len - pos - 8);
            uint32_t n = sz / 2 / ch; out = malloc((size_t)n * 2);
            for (uint32_t i = 0; i < n; i++) out[i] = (int16_t)(c[i * ch * 2] | c[i * ch * 2 + 1] << 8);
            *count = n; break;
        }
        pos += 8 + (long)sz + (sz & 1);
    }
    free(d);
    if (!out) fprintf(stderr, "keine Audiodaten gefunden\n");
    return out;
}

static void feed(const int16_t *s, uint32_t n)
{
    static int32_t blk[4096];
    while (n) {
        uint32_t m = n < 4096 ? n : 4096;
        for (uint32_t i = 0; i < m; i++) blk[i] = (int32_t)s[i] * 256;       /* 16 -> 24 bit */
        nc_push24(&g_nc, blk, m); g_pushed += m;
        nc_poll(&g_nc);
        s += m; n -= m;
    }
}

#ifdef _WIN32
/* ---------------------------------------------------------------- Mikrofon (WinMM) */
static BOOL WINAPI on_ctrl(DWORD t) { (void)t; g_stop = 1; return TRUE; }
#define NBUF 20

static void list_devices(void)
{
    UINT n = waveInGetNumDevs();
    printf("Aufnahmegeraete (--device N):\n");
    for (UINT i = 0; i < n; i++) { WAVEINCAPSA c; if (waveInGetDevCaps(i, &c, sizeof c) == MMSYSERR_NOERROR) printf("  %u: %s\n", i, c.szPname); }
    if (!n) printf("  (keine gefunden)\n");
}

/* Aufnahme starten; calibrate: nur Pegel messen */
static int run_mic(int dev, int calibrate)
{
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, g_fs, g_fs * 2, 2, 16, 0 };
    HANDLE ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    HWAVEIN hwi; MMRESULT r = waveInOpen(&hwi, dev < 0 ? WAVE_MAPPER : (UINT)dev, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) { fprintf(stderr, "Mikrofon kann nicht geoeffnet werden (Fehler %u). Mit --list Geraete pruefen, ggf. --rate 44100.\n", r); return 1; }
    uint32_t blen = g_fs / 10;                                              /* 100-ms-Puffer */
    WAVEHDR hdr[NBUF]; int16_t *buf[NBUF];
    for (int i = 0; i < NBUF; i++) {
        buf[i] = malloc(blen * 2); memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (LPSTR)buf[i]; hdr[i].dwBufferLength = blen * 2;
        waveInPrepareHeader(hwi, &hdr[i], sizeof hdr[i]); waveInAddBuffer(hwi, &hdr[i], sizeof hdr[i]);
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    waveInStart(hwi);
    printf(calibrate ? "Kalibrierung: Kalibrator (94 dB, 1 kHz) ans Mikrofon halten ... 6 s\n"
                     : "Ueberwachung laeuft (Strg+C beendet). Ausgabe: %s\n", g_out);
    int next = 0; uint64_t total = 0;
    double sum = 0, sumsq = 0; uint64_t ncal = 0;
    while (!g_stop) {
        WaitForSingleObject(ev, 200);
        while (hdr[next].dwFlags & WHDR_DONE) {
            uint32_t n = hdr[next].dwBytesRecorded / 2;
            if (calibrate) {
                if (total >= g_fs)                                         /* erste Sekunde (Einschwingen) verwerfen */
                    for (uint32_t i = 0; i < n; i++) { sum += buf[next][i]; sumsq += (double)buf[next][i] * buf[next][i]; ncal++; }
                total += n;
            } else feed(buf[next], n);
            waveInAddBuffer(hwi, &hdr[next], sizeof hdr[next]);
            next = (next + 1) % NBUF;
            if (!calibrate) ui_tick();
        }
        if (calibrate && total >= (uint64_t)g_fs * 6) break;
    }
    waveInStop(hwi); waveInReset(hwi);
    for (int i = 0; i < NBUF; i++) { waveInUnprepareHeader(hwi, &hdr[i], sizeof hdr[i]); free(buf[i]); }
    waveInClose(hwi);
    if (calibrate) {
        if (!ncal) { fprintf(stderr, "keine Daten\n"); return 1; }
        double m = sum / ncal, rms = sqrt(sumsq / ncal - m * m);
        double sens = 20.0 * log10(rms * 1.41421356 / 32768.0);              /* Spitzenwert-Konvention wie im Datenblatt */
        printf("Effektivwert %.1f Counts -> Empfindlichkeit %.1f dBFS bei 94 dB SPL\nStarten mit:  --sens %.1f\n", rms, sens, sens);
        if (rms < 5.0) printf("Hinweis: Signal sehr schwach - Kalibrator am Mikrofon? Windows-Eingangspegel pruefen.\n");
        return 0;
    }
    nc_flush(&g_nc);
    printf("\nBeendet. Gespeicherte Ereignisse: %lu\n", (unsigned long)g_nc.events_total);
    return 0;
}
#endif

#if !defined(_WIN32) && defined(HAVE_ALSA)
/* ---------------------------------------------------------------- Mikrofon (ALSA, Linux) */
static void on_sig(int s) { (void)s; g_stop = 1; }

static void list_alsa(void)
{
    void **hints;
    printf("Aufnahmegeraete (--device NAME, z.B. default oder plughw:1,0):\n");
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return;
    for (void **h = hints; *h; h++) {
        char *name = snd_device_name_get_hint(*h, "NAME"), *io = snd_device_name_get_hint(*h, "IOID"), *desc = snd_device_name_get_hint(*h, "DESC");
        if (name && (!io || !strcmp(io, "Input"))) {
            printf("  %s", name);
            if (desc) { for (char *c = desc; *c; c++) if (*c == '\n') *c = ' '; printf("   (%s)", desc); }
            printf("\n");
        }
        free(name); free(io); free(desc);
    }
    snd_device_name_free_hint(hints);
}

static int run_alsa(const char *dev, int calibrate)
{
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0) < 0 ||
        snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, g_fs, 1, 500000) < 0) {
        fprintf(stderr, "Mikrofon '%s' kann nicht geoeffnet werden. Mit --list Geraete pruefen, ggf. --rate 44100.\n", dev);
        return 1;
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    uint32_t blen = g_fs / 10; int16_t *buf = malloc(blen * 2);
    printf(calibrate ? "Kalibrierung: Kalibrator (94 dB, 1 kHz) ans Mikrofon halten ... 6 s\n"
                     : "Ueberwachung laeuft (Strg+C beendet). Ausgabe: %s\n", g_out);
    uint64_t total = 0, ncal = 0; double sum = 0, sumsq = 0;
    while (!g_stop) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, blen);
        if (n < 0) { snd_pcm_recover(pcm, (int)n, 1); continue; }           /* Ueberlauf (xrun) o. ae. */
        if (calibrate) {
            if (total >= g_fs) for (snd_pcm_sframes_t i = 0; i < n; i++) { sum += buf[i]; sumsq += (double)buf[i] * buf[i]; ncal++; }
            total += (uint64_t)n;
            if (total >= (uint64_t)g_fs * 6) break;
        } else {
            feed(buf, (uint32_t)n);
            ui_tick();
        }
    }
    snd_pcm_close(pcm); free(buf);
    if (calibrate) {
        if (!ncal) { fprintf(stderr, "keine Daten\n"); return 1; }
        double m = sum / ncal, rms = sqrt(sumsq / ncal - m * m), sens = 20.0 * log10(rms * 1.41421356 / 32768.0);
        printf("Effektivwert %.1f Counts -> Empfindlichkeit %.1f dBFS bei 94 dB SPL\nStarten mit:  --sens %.1f\n", rms, sens, sens);
        return 0;
    }
    nc_flush(&g_nc);
    printf("\nBeendet. Gespeicherte Ereignisse: %lu\n", (unsigned long)g_nc.events_total);
    return 0;
}
#endif

/* ---------------------------------------------------------------- main */
int main(int argc, char **argv)
{
    g_argc = argc; g_argv = argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (flag("--help") || flag("-h") || argc == 1) {
        printf("Laermtracker (Windows, USB-Mikrofon)\n"
               "  --list                  Aufnahmegeraete anzeigen\n"
               "  --device N|NAME         Geraet waehlen (Windows: Nummer, Linux: ALSA-Name, z.B. plughw:1,0; Standard: Systemstandard)\n"
               "  --sens dBFS             Mikrofonempfindlichkeit bei 94 dB SPL (Standard -26 = ICS-43434); mit --calibrate bestimmen\n"
               "  --calibrate             Empfindlichkeit mit 94-dB-Kalibrator messen\n"
               "  --out DIR               Ausgabeordner auf der Festplatte (Standard: Benutzerordner/Laermtracker)\n"
               "  --rate HZ               Abtastrate (Standard 48000, Vielfaches von 100)\n"
               "  --wav-in DATEI [--start \"JJJJ-MM-TT hh:mm:ss\"]   statt Mikrofon eine WAV-Datei analysieren\n"
               "  --trig-min dBA (50)  --trig-delta dB (12)  --pre-ms (1000)  --post-ms (3000)  --max-ms (60000)\n"
               "  --threshold dBA         fester Ausloesepegel (wie --trig-min dBA --trig-delta 0); Ausloesung ab diesem LAeq(10 ms) in dB(A)\n"
               "  --status-s N            Pegelanzeige alle N Sekunden (0 = aus, Standard 1)\n"
               "  --log                   zeilenweise Ausgabe mit Uhrzeit statt ueberschreibender Zeile (systemd, Logdateien)\n"
               "  --slam dBA (75)  --night-start h (22)  --night-end h (6)  --lim-night dBA (42)  --lim-day dBA (55)\n"
               "  --recalibrate dB        vorhandene Ergebnisse im --out-Ordner nachtraeglich um dB korrigieren (CSV, Plots, Stoerungen neu; Original: EVENTS_ALT.CSV)\n"
               "  --keep-sonst 0|1        nicht impulshafte Ereignisse (Stimmen, Brummen) behalten (Standard 1)\n"
               "Hinweis: Automatische Verstaerkung (AGC) und Klangverbesserungen des Mikrofons in den Windows-Soundeinstellungen\n"
               "abschalten, sonst sind die Pegel unbrauchbar.\n");
        return 0;
    }
#ifdef _WIN32
    if (flag("--list")) { list_devices(); return 0; }
#elif defined(HAVE_ALSA)
    if (flag("--list")) { list_alsa(); return 0; }
#endif
    { const char *up = getenv("USERPROFILE"); if (!up) up = getenv("HOME");                       /* Standard: Festplatte, Benutzerordner */
      if (up && strlen(up) < 200) snprintf(g_out, sizeof g_out, "%s" PSEP "Laermtracker", up); }
    if (opt("--out")) snprintf(g_out, sizeof g_out, "%s", opt("--out"));
    g_fs = (uint32_t)optd("--rate", 48000);
    if (g_fs < 8000 || g_fs % 100) { fprintf(stderr, "--rate muss >= 8000 und ein Vielfaches von 100 sein\n"); return 1; }
    const char *wavin = opt("--wav-in");
    int16_t *wav = NULL; uint32_t wav_n = 0;
    if (wavin) {
        uint32_t r = 0; wav = read_wav(wavin, &r, &wav_n); if (!wav) return 1;
        g_fs = r; g_sim = 1;
        int y, mo, d, h, mi, s;
        const char *st = opt("--start");
        if (st && sscanf(st, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) g_sim_start_ms = nc_ms_from_civil(y, mo, d, h, mi, s, 0);
        else { g_sim = 0; g_sim_start_ms = io_now(NULL); g_sim = 1; }
        if (g_fs % 100) { fprintf(stderr, "Abtastrate %u Hz nicht unterstuetzt (Vielfaches von 100 noetig)\n", (unsigned)g_fs); return 1; }
    }

    double sens = optd("--sens", NC_MIC_SENS_DBFS);
    g_cal_db = (float)(-sens - 26.0);                                       /* Kern geht von -26 dBFS @ 94 dB aus */
    nc_config_t cfg; nc_default_config(&cfg);
    cfg.fs = g_fs; cfg.cal_offset_db = g_cal_db;
    cfg.trig_min_dba = (float)optd("--trig-min", cfg.trig_min_dba);
    cfg.trig_delta_db = (float)optd("--trig-delta", cfg.trig_delta_db);
    cfg.pre_ms = (uint32_t)optd("--pre-ms", cfg.pre_ms);
    cfg.post_ms = (uint32_t)optd("--post-ms", cfg.post_ms);
    cfg.max_event_ms = (uint32_t)optd("--max-ms", cfg.max_event_ms);
    cfg.slam_lafmax_dba = (float)optd("--slam", cfg.slam_lafmax_dba);
    cfg.night_start_h = (uint8_t)optd("--night-start", cfg.night_start_h);
    cfg.night_end_h = (uint8_t)optd("--night-end", cfg.night_end_h);
    cfg.lim_night_dba = (float)optd("--lim-night", cfg.lim_night_dba);
    cfg.lim_day_dba = (float)optd("--lim-day", cfg.lim_day_dba);
    cfg.keep_sonst = (uint8_t)optd("--keep-sonst", 1);
    if (opt("--threshold")) { cfg.trig_min_dba = (float)optd("--threshold", 50); cfg.trig_delta_db = 0.0f; }   /* fester Ausloesepegel */
    g_status_ms = (int)(optd("--status-s", 1.0) * 1000.0); g_log_lines = flag("--log");

    uint32_t ring_len = g_fs * 4;                                           /* 4 s Ringpuffer */
    int16_t *ring = malloc((size_t)ring_len * 2);
    if (!ring) return 1;
    nc_io_t io = { NULL, io_now, io_mkdir, io_open, io_write, io_seek, io_close, io_read, io_remove };
    nc_init(&g_nc, &cfg, &io, ring, ring_len);
    MKDIR(g_out);

    if (opt("--recalibrate")) {                                         /* vorhandene Ergebnisse um X dB umrechnen */
        float dl = (float)optd("--recalibrate", 0.0);
        nc_recal_t r; int rc = nc_recalibrate(&g_nc, dl, &r);
        if (rc) fprintf(stderr, "%s\n", rc == -1 ? "Keine EVENTS.CSV im Ausgabeordner gefunden." : "Keine Ereignisse in EVENTS.CSV.");
        else printf("Pegel um %+.1f dB korrigiert: %lu Ereignisse, %lu Ereignisplots, %lu Tagesdiagramme neu erzeugt. Original: EVENTS_ALT.CSV\n",
                    dl, (unsigned long)r.rows, (unsigned long)r.svgs, (unsigned long)r.days);
        free(ring); return rc ? 1 : 0;
    }

    if (wavin) {
        if (!opt("--status-s")) g_status_ms = 0;                                  /* bei WAV-Analyse nur Ereignisse melden */
        printf("Analysiere %s (%u Hz, %.1f s), Empfindlichkeit %.1f dBFS@94 dB\n", wavin, (unsigned)g_fs, wav_n / (double)g_fs, sens);
        for (uint32_t i = 0; i < wav_n; i += g_fs / 10) {                   /* in 100-ms-Stuecken */
            uint32_t m = wav_n - i < g_fs / 10 ? wav_n - i : g_fs / 10;
            feed(wav + i, m); ui_tick();
        }
        nc_flush(&g_nc); ui_tick();
        printf("Fertig. Gespeichert: %lu, verworfen: %lu. Ausgabe in %s\n", (unsigned long)g_nc.events_total, (unsigned long)g_nc.events_dropped, g_out);
        free(wav); free(ring);
        return 0;
    }
#ifdef _WIN32
    int rc = run_mic(flag("--device") || opt("--device") ? (int)optd("--device", -1) : -1, flag("--calibrate"));
    free(ring);
    return rc;
#elif defined(HAVE_ALSA)
    int rc = run_alsa(opt("--device") ? opt("--device") : "default", flag("--calibrate"));
    free(ring);
    return rc;
#else
    fprintf(stderr, "Diese Programmversion wurde ohne Mikrofonunterstuetzung gebaut (Linux: mit -DHAVE_ALSA -lasound bauen); hier nur --wav-in.\n");
    free(ring);
    return 1;
#endif
}
