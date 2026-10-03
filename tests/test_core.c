/*
 * Host-Test fuer core/noise_core.c: synthetische Wohnung mit Tuer- und Knallereignissen.
 * Referenzpegel werden unabhaengig (analytische A-Bewertung nach IEC 61672, Doppelpraezision) berechnet.
 * Aufruf: test_core <ausgabeverzeichnis>
 */
#include "../core/noise_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#define FS 32000
#define TOTAL_S 90
#define NCOMP 24
#define TWO_PI 6.283185307179586

static char g_out[256];
static nc_t g_nc;
static int failures;

/* ---- analytische A-Bewertung ---- */
static double a_lin(double f)
{
    double f2 = f * f, c1 = 20.598997 * 20.598997, c2 = 107.65265 * 107.65265, c3 = 737.86223 * 737.86223, c4 = 12194.217 * 12194.217;
    double ra = c4 * f2 * f2 / ((f2 + c1) * sqrt((f2 + c2) * (f2 + c3)) * (f2 + c4));
    return ra / 0.891250938;                                  /* A(1 kHz) = 1 */
}

/* ---- I/O ueber stdio ---- */
static void path_of(char *o, size_t sz, const char *p) { snprintf(o, sz, "%s%s", g_out, p); }
static int64_t t_base;
static int64_t io_now(void *u) { (void)u; return t_base + (int64_t)(g_nc.wr * 1000ull / FS); }
static int io_mkdir(void *u, const char *p) { (void)u; char b[300]; path_of(b, sizeof b, p); MKDIR(b); return 0; }
static void *io_open(void *u, const char *p, char m) { (void)u; char b[300]; path_of(b, sizeof b, p);
    return fopen(b, m == 'r' ? "rb" : (m == 'a' ? "ab" : "wb")); }
static int io_write(void *u, void *h, const void *b, uint32_t n) { (void)u; return fwrite(b, 1, n, (FILE *)h) == n ? 0 : -1; }
static int io_seek(void *u, void *h, uint32_t p) { (void)u; return fseek((FILE *)h, (long)p, SEEK_SET); }
static int io_close(void *u, void *h) { (void)u; return fclose((FILE *)h); }

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* ---- Signalerzeugung ---- */
static double fc[NCOMP], ph[NCOMP], Ac[NCOMP];
static float *raw, *wgt;                       /* Zaehlwerte roh / A-bewertet (Referenz) */
static double la_unit;                         /* A-Pegel von sum(sin) mit Amplitude 1 [dB SPL] */
static double ref_peak, ref_rms2;

static double k_for(double target_dba) { return pow(10.0, (target_dba - la_unit) / 20.0); }

static void add_signal(double t0, double dur, double target_dba, int shape, double tau)
{
    double k = k_for(target_dba);
    long i0 = (long)(t0 * FS), n = (long)(dur * FS);
    for (long i = 0; i < n && i0 + i < (long)TOTAL_S * FS; i++) {
        double t = (double)i / FS, env = 1.0;
        if (shape == 0) env = (1.0 - exp(-t / 0.002)) * exp(-t / tau);                 /* Tuer/Knall */
        else if (shape == 1) env = 0.5 - 0.5 * cos(TWO_PI * t / dur);                  /* langsames Anschwellen (Sprache-aehnlich) */
        else env = t < 0.005 ? t / 0.005 : (t > dur - 0.005 ? (dur - t) / 0.005 : 1.0); /* Ton */
        double r = 0, w = 0;
        if (shape == 2) { double s = sin(TWO_PI * 1000.0 * (i0 + i) / FS); r = s; w = s * a_lin(1000.0); }
        else for (int c = 0; c < NCOMP; c++) {
            double s = sin(TWO_PI * fc[c] * (i0 + i) / FS + ph[c]);
            r += s; w += s * Ac[c];
        }
        raw[i0 + i] += (float)(k * env * r);
        wgt[i0 + i] += (float)(k * env * w);
    }
}

/* Referenz: LAeq/LAFmax/Lpeak ueber Fenster [ta, tb] */
static double ref_laeq(double ta, double tb)
{
    long a = (long)(ta * FS), b = (long)(tb * FS); double s = 0;
    for (long i = a; i < b; i++) s += (double)wgt[i] * wgt[i];
    return 94.0 + 10.0 * log10(s / (b - a) / ref_rms2);
}
static double ref_lafmax(double ta, double tb)
{
    long fl = FS / 100, a = (long)(ta * FS) / fl * fl, b = (long)(tb * FS);
    double al = 1.0 - exp(-0.01 / 0.125), e = -1, mx = -1e9;
    for (long i = (a > 200 * fl ? a - 200 * fl : 0); i + fl <= b; i += fl) {
        double s = 0; for (long j = 0; j < fl; j++) s += (double)wgt[i + j] * wgt[i + j];
        s = s / fl; if (s < 1e-3) s = 1e-3;
        e = e < 0 ? s : e + al * (s - e);
        double L = 94.0 + 10.0 * log10(e / ref_rms2);
        if (i >= a && L > mx) mx = L;
    }
    return mx;
}
static double ref_lpeak(double ta, double tb)
{
    double m = 1; for (long i = (long)(ta * FS); i < (long)(tb * FS); i++) if (fabs(raw[i]) > m) m = fabs(raw[i]);
    return 94.0 + 20.0 * log10(m / ref_peak);
}

typedef struct { double t0, dur, dba, tau; int shape; const char *cls; } ev_t;

static void check(int ok, const char *fmt, ...)
{
    (void)fmt; if (!ok) failures++;
}
#define CHECK(cond, ...) do { int ok_ = (cond); printf("  [%s] ", ok_ ? "OK " : "FAIL"); printf(__VA_ARGS__); printf("\n"); check(ok_, ""); } while (0)

int main(int argc, char **argv)
{
    snprintf(g_out, sizeof g_out, "%s", argc > 1 ? argv[1] : "out");
    MKDIR(g_out);
    char csvp[300]; path_of(csvp, sizeof csvp, "/EVENTS.CSV"); remove(csvp);

    /* 1) A-Bewertungsfilter gegen analytische Kurve */
    printf("A-Bewertung (digital vs. IEC 61672):\n");
    const float fr[] = { 31.5f, 63, 125, 250, 500, 1000, 2000, 4000, 8000 };
    for (unsigned i = 0; i < sizeof fr / sizeof *fr; i++) {
        double ref = 20 * log10(a_lin(fr[i])), got = nc_a_weight_response_db(fr[i], FS);
        CHECK(fabs(ref - got) < (fr[i] > 4000 ? 2.0 : 0.5), "%7.1f Hz: IEC %+6.2f dB, Filter %+6.2f dB", fr[i], ref, got);
    }

    /* 2) Signal vorbereiten */
    ref_peak = NC_FULLSCALE_24BIT * pow(10.0, NC_MIC_SENS_DBFS / 20.0); ref_rms2 = ref_peak * ref_peak / 2;
    unsigned seed = 12345; double sw = 0;
    for (int c = 0; c < NCOMP; c++) {
        fc[c] = 100.0 * pow(60.0, c / (double)(NCOMP - 1)) * (1.0 + 0.013 * (c % 3));     /* 100 Hz ... 6 kHz */
        seed = seed * 1664525u + 1013904223u; ph[c] = (seed >> 8) / 16777216.0 * TWO_PI;
        Ac[c] = a_lin(fc[c]); sw += Ac[c] * Ac[c] / 2.0;
    }
    la_unit = 94.0 + 10.0 * log10(sw / ref_rms2);
    size_t N = (size_t)TOTAL_S * FS;
    raw = calloc(N, sizeof(float)); wgt = calloc(N, sizeof(float));
    if (!raw || !wgt) return 2;

    /* Hintergrund: konstanter Mehrton-Teppich, 33 dB(A) */
    { double k = k_for(33.0);
      for (size_t i = 0; i < N; i++) { double r = 0, w = 0;
          for (int c = 0; c < NCOMP; c++) { double s = sin(TWO_PI * fc[c] * i / FS + ph[c] + 1.0); r += s; w += s * Ac[c]; }
          raw[i] = (float)(k * r); wgt[i] = (float)(k * w); } }

    ev_t ev[] = {
        { 10.0, 0.6, 62.0, 0.05, 0, "TUER"  },               /* Tuer ins Schloss */
        { 25.0, 1.5, 88.0, 0.08, 0, "KNALL" },               /* Tuer zuknallen */
        { 27.0, 1.5, 70.0, 0.06, 0, "KNALL" },               /* Nachschlag nach 2 s -> gleiches Ereignis */
        { 45.0, 4.0, 62.0, 0.00, 1, "SONST" },               /* langsames Anschwellen */
        { 65.0, 2.0, 80.0, 0.00, 2, "KNALL" },               /* 1-kHz-Ton, Kalibrierung */
    };
    const int nev = sizeof ev / sizeof *ev;
    for (int i = 0; i < nev; i++) add_signal(ev[i].t0, ev[i].dur, ev[i].dba, ev[i].shape, ev[i].tau);

    /* 3) Kern laufen lassen */
    nc_config_t cfg; nc_default_config(&cfg);
    nc_io_t io = { NULL, io_now, io_mkdir, io_open, io_write, io_seek, io_close };
    static int16_t ring[48000];
    nc_init(&g_nc, &cfg, &io, ring, 48000);
    t_base = (days_from_civil(2026, 10, 3) * 86400 + 14 * 3600 + 30 * 60) * 1000;
    static int32_t blk[512];
    for (size_t i = 0; i < N; i += 512) {
        size_t m = N - i < 512 ? N - i : 512;
        for (size_t j = 0; j < m; j++) {
            double v = raw[i + j];
            v = v > 8388607 ? 8388607 : (v < -8388608 ? -8388608 : v);
            blk[j] = (int32_t)lrint(v);
        }
        nc_process(&g_nc, blk, (uint32_t)m);
    }
    nc_flush(&g_nc);

    /* 4) Ergebnisse (EVENTS.CSV) gegen Referenz */
    printf("\nEreignisse (erkannt: %lu, Ueberlaeufe: %lu):\n", (unsigned long)g_nc.events_total, (unsigned long)g_nc.overruns);
    FILE *f = fopen(csvp, "r");
    CHECK(f != NULL, "EVENTS.CSV vorhanden");
    if (!f) return 1;
    char line[512]; fgets(line, sizeof line, f);
    /* erwartete Gruppen: Ereignis 2+3 verschmelzen */
    struct { double t0, t1; const char *cls; } exp_[] = { {10.0, 10.6, "TUER"}, {25.0, 28.5, "KNALL"}, {45.0, 49.0, "SONST"}, {65.0, 67.0, "KNALL"} };
    int row = 0;
    while (fgets(line, sizeof line, f)) {
        char d[16], tm[16], cls[8], file[64]; double act, laeq, lafmax, lpk, lae, bg; int ovr = 0;
        for (char *p = line; *p; p++) if (*p == ';') *p = ' ';
        if (sscanf(line, "%15s %15s %7s %lf %lf %lf %lf %lf %lf %63s %d", d, tm, cls, &act, &laeq, &lafmax, &lpk, &lae, &bg, file, &ovr) < 10) continue;
        int hh, mm, ss, ms; sscanf(tm, "%d:%d:%d.%d", &hh, &mm, &ss, &ms);
        double fstart = hh * 3600 + mm * 60 + ss + ms / 1000.0 - (14 * 3600 + 30 * 60);
        printf("Zeile %d: %s %s %s aktiv %.2fs LAeq %.1f LAFmax %.1f Lpeak %.1f LAE %.1f bg %.1f\n", row, d, tm, cls, act, laeq, lafmax, lpk, lae, bg);
        if (row >= 4) { row++; continue; }
        double ta = fstart + 1.0, tb = ta + act;               /* Ausloesung = Dateianfang + 1 s Vorlauf */
        CHECK(!strcmp(cls, exp_[row].cls), "Klasse %s (erwartet %s)", cls, exp_[row].cls);
        CHECK(ta > exp_[row].t0 - 0.3 && ta < exp_[row].t1, "Beginn %.2f s im Fenster [%.1f, %.1f]", ta, exp_[row].t0 - 0.3, exp_[row].t1);
        double wa = exp_[row].t0 - 0.2, wb = exp_[row].t1 + 4.0;
        double rlf = ref_lafmax(wa, wb), rlp = ref_lpeak(wa, wb), rle = ref_laeq(ta, tb);
        CHECK(fabs(rlf - lafmax) < 1.0, "LAFmax %.1f dB(A), Referenz %.1f", lafmax, rlf);
        CHECK(fabs(rlp - lpk) < 0.6,    "Lpeak  %.1f dB,    Referenz %.1f", lpk, rlp);
        CHECK(fabs(rle - laeq) < 1.0,   "LAeq   %.1f dB(A), Referenz %.1f", laeq, rle);
        CHECK(fabs(lae - (laeq + 10 * log10(act))) < 0.3, "LAE = LAeq + 10 lg T konsistent (%.1f)", lae);
        if (row == 3) CHECK(fabs(laeq - 80.0) < 0.6, "Kalibrierung: 1-kHz-Ton 80 dB SPL -> LAeq %.2f", laeq);
        row++;
    }
    fclose(f);
    CHECK(row == 4, "genau 4 Ereignisse erkannt (gefunden: %d)", row);
    free(raw); free(wgt);
    printf("\n%s (%d Fehler)\n", failures ? "FEHLGESCHLAGEN" : "ALLE TESTS BESTANDEN", failures);
    return failures != 0;
}
