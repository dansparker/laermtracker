/*
 * Grenzfall- und Robustheitstests fuer core/noise_core.c (Host):
 *   T1 keine Fehlausloesung bei 5 min schwankendem Hintergrund
 *   T2 Audiodaten bit-genau (auch bei 400 ms Hauptschleifen-Stillstand)
 *   T3 Positionszaehler-Ueberlauf (wr nahe Modul M)
 *   T4 Stillstand laenger als der Ringpuffer: Verlust wird gezaehlt, Datei bleibt gueltig
 *   T5 Dauerlaerm: harte Obergrenze 60 s, danach neues Ereignis
 *   T6 Mitternachtswechsel (Ordner, Tagesdatei)
 *   T7 SD-Fehler beim Oeffnen: kein Absturz, spaeteres Ereignis wird gespeichert
 *   T8 > 300 Ereignisse an einem Tag: Tagesdiagramm wird begrenzt
 * Aufruf: test_edge <ausgabeverzeichnis>
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
#define RING 48000

static char g_root[200], g_cur[256];
static int failures;
static uint64_t g_pushed;
static int64_t g_base;
static int g_failopen;
static nc_t g_nc;
static int16_t g_ring[RING];

#define CHECK(cond, ...) do { int ok_ = (cond); printf("  [%s] ", ok_ ? "OK " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) failures++; } while (0)

/* ---- I/O ---- */
static int64_t io_now(void *u) { (void)u; return g_base + (int64_t)(g_pushed * 1000ull / FS); }
static void full(char *o, size_t sz, const char *p) { snprintf(o, sz, "%s%s", g_cur, p); }
static int io_mkdir(void *u, const char *p) { (void)u; char b[400]; full(b, sizeof b, p); MKDIR(b); return 0; }
static void *io_open(void *u, const char *p, char m)
{
    (void)u; char b[400]; full(b, sizeof b, p);
    size_t l = strlen(p);
    if (m == 'w' && g_failopen > 0 && l > 4 && !strcmp(p + l - 4, ".WAV")) { g_failopen--; return NULL; }
    return fopen(b, m == 'r' ? "rb" : (m == 'a' ? "ab" : "wb"));
}
static void inject_noise(void);
static int g_inject;
static int io_write(void *u, void *h, const void *b, uint32_t n) { (void)u; if (g_inject) inject_noise(); return fwrite(b, 1, n, (FILE *)h) == n ? 0 : -1; }
static int io_seek(void *u, void *h, uint32_t p) { (void)u; return fseek((FILE *)h, (long)p, SEEK_SET); }
static int io_close(void *u, void *h) { (void)u; return fclose((FILE *)h); }
static int32_t io_read(void *u, void *h, void *b, uint32_t n) { (void)u; return (int32_t)fread(b, 1, n, (FILE *)h); }
static int io_remove(void *u, const char *p) { (void)u; char b[400]; full(b, sizeof b, p); return remove(b); }
static int g_keep_sonst = 1;

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static int64_t ms_of(int y, int mo, int d, int h, int mi, int s) { return (days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s) * 1000; }

/* ---- Signal ---- */
typedef struct { double t0, dur, dba, tau, sig, att; } burst_t;
static burst_t *bursts; static int nb, curb;
static double bg_dba, bg_mod_db, sig_bg, g2, ref_rms2;
static uint32_t rng;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0 - 0.5; }
static double gauss(void) { return (urand() + urand() + urand() + urand()) / sqrt(4.0 / 12.0); }
static double sigma_for(double dba) { return sqrt(ref_rms2 * pow(10.0, (dba - 94.0) / 10.0) / g2); }

static void gen_block(uint64_t i0, uint32_t n, int32_t *out)
{
    for (uint32_t k = 0; k < n; k++) {
        uint64_t i = i0 + k; double t = (double)i / FS;
        double x = sig_bg * pow(10.0, bg_mod_db * sin(6.283185307 * 0.05 * t) / 20.0) * gauss();
        while (curb < nb && t > bursts[curb].t0 + bursts[curb].dur) curb++;
        if (curb < nb && t >= bursts[curb].t0) {
            double e = t - bursts[curb].t0;
            x += bursts[curb].sig * (1.0 - exp(-e / (bursts[curb].att > 0 ? bursts[curb].att : 0.002))) * exp(-e / bursts[curb].tau) * gauss();
        }
        long v = lrint(x); out[k] = (int32_t)(v > 8388607 ? 8388607 : (v < -8388608 ? -8388608 : v));
    }
}

static uint64_t g_i;
/* Simuliert Schaltrauschen der SD-Karte: waehrend jedes Schreibzugriffs laufen 20 ms Signal mit +66 dB(A)-Rauschen weiter ein
 * (wie der Audio-Interrupt, der waehrend des blockierenden Zugriffs weiterarbeitet) */
static void inject_noise(void)
{
    static int32_t nb[320];
    gen_block(g_i, 320, nb);
    double sg = sigma_for(66.0);
    for (int k = 0; k < 320; k++) { long v = nb[k] + lrint(sg * gauss()); nb[k] = (int32_t)(v > 8388607 ? 8388607 : (v < -8388608 ? -8388608 : v)); }
    nc_push24(&g_nc, nb, 320); g_pushed += 320; g_i += 320;
}

typedef struct { double seconds, stall_t0, stall_len; int32_t *rec; uint32_t wrap_offset; } sim_t;

static void run_sim(const char *name, const sim_t *o, burst_t *b, int n_b, int64_t base, double bg, double mod)
{
    snprintf(g_cur, sizeof g_cur, "%s/%s", g_root, name); MKDIR(g_cur);
    char p[400]; snprintf(p, sizeof p, "%s/EVENTS.CSV", g_cur); remove(p);
    bursts = b; nb = n_b; curb = 0; for (int i = 0; i < nb; i++) bursts[i].sig = sigma_for(bursts[i].dba);
    bg_dba = bg; bg_mod_db = mod; sig_bg = sigma_for(bg); rng = 7; g_pushed = 0; g_base = base;
    nc_config_t cfg; nc_default_config(&cfg);
    nc_io_t io = { NULL, io_now, io_mkdir, io_open, io_write, io_seek, io_close, io_read, io_remove };
    cfg.keep_sonst = (uint8_t)g_keep_sonst;
    nc_init(&g_nc, &cfg, &io, g_ring, RING);
    if (o->wrap_offset) { g_nc.wr = g_nc.M - o->wrap_offset; g_nc.widx = g_nc.wr % g_nc.ring_len; }
    static int32_t blk[256];
    uint64_t N = (uint64_t)(o->seconds * FS);
    g_i = 0;
    while (g_i < N) {
        uint32_t m = (uint32_t)(N - g_i < 256 ? N - g_i : 256);
        gen_block(g_i, m, blk);
        if (o->rec) memcpy(o->rec + g_i, blk, m * sizeof(int32_t));
        nc_push24(&g_nc, blk, m); g_pushed += m;
        double t = (double)g_i / FS;
        g_i += m;
        if (!(o->stall_len > 0 && t >= o->stall_t0 && t < o->stall_t0 + o->stall_len)) nc_poll(&g_nc);
    }
    nc_poll(&g_nc); nc_flush(&g_nc);
}

static int count_rows(const char *rel)
{
    char p[400]; snprintf(p, sizeof p, "%s%s", g_cur, rel);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    int n = 0, c; while ((c = fgetc(f)) != EOF) n += c == '\n';
    fclose(f); return n > 0 ? n - 1 : 0;
}
static int file_exists(const char *rel) { char p[400]; snprintf(p, sizeof p, "%s%s", g_cur, rel); FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }
static long file_size(const char *rel) { char p[400]; snprintf(p, sizeof p, "%s%s", g_cur, rel); FILE *f = fopen(p, "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }

/* Spalte col der n-ten Datenzeile von EVENTS.CSV */
static int csv_col(int row, int col, char *out, size_t sz)
{
    char p[400], line[512]; snprintf(p, sizeof p, "%s/EVENTS.CSV", g_cur);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    int r = -1; while (fgets(line, sizeof line, f)) {
        if (r++ == row) { char *a = line; for (int k = 0; k < col; k++) a = strchr(a, ';') + 1; char *e = a + strcspn(a, ";\n"); *e = 0; snprintf(out, sz, "%s", a); fclose(f); return 0; }
    }
    fclose(f); return -1;
}
static int csv_file(int row, char *out, size_t sz) { return csv_col(row, 13, out, sz); }

/* WAV-Daten == Eingang? (Versatz wird gesucht) */
static void check_bitexact(const int32_t *rec, uint64_t nrec, double around_s, const char *label)
{
    char rel[64]; CHECK(csv_file(0, rel, sizeof rel) == 0, "%s: Ereignis in CSV", label);
    char p[400]; snprintf(p, sizeof p, "%s%s", g_cur, rel);
    FILE *f = fopen(p, "rb"); if (!f) { CHECK(0, "%s: WAV oeffnen", label); return; }
    uint8_t h[654]; if (fread(h, 1, 654, f) != 654) { CHECK(0, "kurz"); fclose(f); return; }
    uint32_t dbytes = h[650] | h[651] << 8 | h[652] << 16 | (uint32_t)h[653] << 24, ns = dbytes / 2;
    int16_t *w = malloc(dbytes); if (fread(w, 1, dbytes, f) != dbytes) { CHECK(0, "Daten kurz"); free(w); fclose(f); return; } fclose(f);
    int64_t lo = (int64_t)(around_s * FS) - 40000, found = -1; if (lo < 0) lo = 0;
    for (int64_t off = lo; off < lo + 44000 && (uint64_t)off + ns <= nrec; off++) {
        int ok = 1; for (int k = 0; k < 16 && ok; k++) ok = w[k] == (int16_t)(rec[off + k] >> 8);
        if (ok) { found = off; break; }
    }
    uint32_t bad = 0;
    if (found >= 0) for (uint32_t k = 0; k < ns; k++) bad += w[k] != (int16_t)(rec[found + k] >> 8);
    CHECK(found >= 0 && bad == 0, "%s: %u Samples (%.2f s) bit-genau gleich dem Eingang (Versatz %.3f s, %u Abweichungen)",
          label, (unsigned)ns, ns / (double)FS, found / (double)FS, (unsigned)bad);
    free(w);
}

int main(int argc, char **argv)
{
    snprintf(g_root, sizeof g_root, "%s", argc > 1 ? argv[1] : "out_edge"); MKDIR(g_root);
    ref_rms2 = pow(NC_FULLSCALE_24BIT * pow(10.0, NC_MIC_SENS_DBFS / 20.0), 2.0) / 2.0;
    { double s = 0; const int N = 400; for (int k = 1; k <= N; k++) s += pow(10.0, nc_a_weight_response_db((float)(k * (FS / 2.0) / N), FS) / 10.0); g2 = s / N; }
    int64_t B = ms_of(2026, 10, 3, 12, 0, 0);

    printf("T1: 5 min Hintergrund 36 dB(A) +-3 dB Schwankung, keine Ereignisse erwartet\n");
    { sim_t o = { 300, 0, 0, NULL, 0 }; run_sim("t1", &o, NULL, 0, B, 36.0, 3.0);
      CHECK(g_nc.events_total == 0, "Fehlausloesungen: %lu", (unsigned long)g_nc.events_total); }

    printf("T2: Burst bei 5 s, Hauptschleife steht 0,4 s still (6,0-6,4 s)\n");
    { static burst_t b[1]; b[0] = (burst_t){ 5.0, 0.8, 78.0, 0.10, 0 };
      int32_t *rec = malloc(15 * FS * sizeof(int32_t)); sim_t o = { 15, 6.0, 0.4, rec, 0 };
      run_sim("t2", &o, b, 1, B, 33.0, 0.0);
      CHECK(g_nc.events_total == 1 && g_nc.overruns == 0 && g_nc.fq_dropped == 0, "1 Ereignis, Verluste: ring %lu, frames %lu", (unsigned long)g_nc.overruns, (unsigned long)g_nc.fq_dropped);
      check_bitexact(rec, 15ull * FS, 5.0, "T2"); free(rec); }

    printf("T3: wie T2, Positionszaehler startet 20000 Samples vor dem Modul-Ueberlauf\n");
    { static burst_t b[1]; b[0] = (burst_t){ 5.0, 0.8, 78.0, 0.10, 0 };
      int32_t *rec = malloc(15 * FS * sizeof(int32_t)); sim_t o = { 15, 6.0, 0.4, rec, 20000 };
      run_sim("t3", &o, b, 1, B, 33.0, 0.0);
      CHECK(g_nc.events_total == 1 && g_nc.overruns == 0, "1 Ereignis ohne Verlust, wr nach Ueberlauf = %lu", (unsigned long)g_nc.wr);
      check_bitexact(rec, 15ull * FS, 5.0, "T3"); free(rec); }

    printf("T4: Stillstand 1,8 s (laenger als Ringpuffer 1,5 s) mitten im Ereignis\n");
    { static burst_t b[1]; b[0] = (burst_t){ 5.0, 0.8, 78.0, 0.10, 0 }; sim_t o = { 15, 5.4, 1.8, NULL, 0 };
      run_sim("t4", &o, b, 1, B, 33.0, 0.0);
      CHECK(g_nc.events_total >= 1, "Ereignis gespeichert (%lu)", (unsigned long)g_nc.events_total);
      CHECK(g_nc.overruns > 0 || g_nc.fq_dropped > 0, "Verlust wird gezaehlt: ring %lu Samples, frames %lu", (unsigned long)g_nc.overruns, (unsigned long)g_nc.fq_dropped);
      char rel[64]; csv_file(0, rel, sizeof rel); char p[400]; snprintf(p, sizeof p, "%s%s", g_cur, rel); FILE *f = fopen(p, "rb");
      uint8_t h[8] = {0}; if (f) { fread(h, 1, 8, f); fclose(f); }
      uint32_t riff = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
      CHECK(f && (long)riff + 8 == file_size(rel), "WAV-Header konsistent (RIFF %lu + 8 == %ld)", (unsigned long)riff, file_size(rel)); }

    printf("T5: 100 s Dauerlaerm 70 dB(A) ab 5 s: Obergrenze 60 s je Ereignis\n");
    { static burst_t b[1]; b[0] = (burst_t){ 5.0, 100.0, 70.0, 1e9, 0 }; sim_t o = { 110, 0, 0, NULL, 0 };
      run_sim("t5", &o, b, 1, B, 33.0, 0.0);
      char rel[64]; csv_file(0, rel, sizeof rel);
      CHECK(g_nc.events_total >= 2, "Ereignisse: %lu (mind. 2 erwartet)", (unsigned long)g_nc.events_total);
      long sz = file_size(rel);
      CHECK(sz > 0 && sz <= 654 + 66L * FS * 2, "erste WAV-Datei %.1f s (<= 61 s + Vor-/Nachlauf)", (sz - 654) / (2.0 * FS)); }

    printf("T6: Mitternachtswechsel (Start 23:59:57, Bursts bei 2 s und 6 s)\n");
    { static burst_t b[2]; b[0] = (burst_t){ 2.0, 0.5, 78.0, 0.08, 0 }; b[1] = (burst_t){ 6.0, 0.5, 78.0, 0.08, 0 }; sim_t o = { 14, 0, 0, NULL, 0 };
      run_sim("t6", &o, b, 2, ms_of(2026, 10, 3, 23, 59, 57), 33.0, 0.0);
      CHECK(g_nc.events_total == 2, "2 Ereignisse (%lu)", (unsigned long)g_nc.events_total);
      CHECK(file_exists("/261003/TAG.CSV") && file_exists("/261004/TAG.CSV") && file_exists("/261004/TAG.SVG"), "Tagesordner 261003 und 261004 mit TAG.CSV/TAG.SVG");
      CHECK(count_rows("/261003/TAG.CSV") == 1 && count_rows("/261004/TAG.CSV") == 1, "je ein Ereignis pro Tag (%d / %d)", count_rows("/261003/TAG.CSV"), count_rows("/261004/TAG.CSV")); }

    printf("T7: erstes Oeffnen der WAV-Datei schlaegt fehl (SD-Fehler)\n");
    { static burst_t b[2]; b[0] = (burst_t){ 3.0, 0.5, 78.0, 0.08, 0 }; b[1] = (burst_t){ 12.0, 0.5, 78.0, 0.08, 0 }; sim_t o = { 20, 0, 0, NULL, 0 };
      g_failopen = 1; run_sim("t7", &o, b, 2, B, 33.0, 0.0);
      CHECK(g_nc.events_total == 1 && count_rows("/EVENTS.CSV") == 1, "kein Absturz, 2. Ereignis gespeichert (%lu)", (unsigned long)g_nc.events_total); }

    printf("T8: 310 Ereignisse an einem Tag (Tagesdiagramm auf 300 begrenzt)\n");
    { enum { NE = 310 }; static burst_t b[NE]; for (int i = 0; i < NE; i++) b[i] = (burst_t){ 3.0 + 7.0 * i, 0.5, 70.0, 0.06, 0 };
      sim_t o = { 3.0 + 7.0 * NE + 3, 0, 0, NULL, 0 };
      run_sim("t8", &o, b, NE, ms_of(2026, 10, 3, 10, 0, 0), 33.0, 0.0);
      CHECK(g_nc.events_total == NE && count_rows("/EVENTS.CSV") == NE && count_rows("/261003/TAG.CSV") == NE, "%lu Ereignisse, %d Zeilen in EVENTS.CSV", (unsigned long)g_nc.events_total, count_rows("/EVENTS.CSV"));
      char p[400]; snprintf(p, sizeof p, "%s/261003/TAG.SVG", g_cur); FILE *f = fopen(p, "rb"); char *buf = calloc(1, 400000); size_t n = f ? fread(buf, 1, 399999, f) : 0; if (f) fclose(f);
      CHECK(n > 0 && strstr(buf, "ersten Ereignisse") != NULL && strstr(buf, "300 Ereignisse") != NULL, "Tagesplot nennt 300 Ereignisse und den Hinweis auf die Begrenzung (%zu Bytes)", n); free(buf); }

    printf("T9: keep_sonst=0 - abrupte Tuer bleibt, langsam anschwellendes Geraeusch (SONST) wird geloescht\n");
    { static burst_t b[2]; b[0] = (burst_t){ 3.0, 0.5, 78.0, 0.08, 0, 0 }; b[1] = (burst_t){ 15.0, 3.0, 66.0, 2.0, 0, 1.0 }; sim_t o = { 25, 0, 0, NULL, 0 };
      g_keep_sonst = 0; run_sim("t9", &o, b, 2, B, 33.0, 0.0); g_keep_sonst = 1;
      CHECK(g_nc.events_total == 1 && g_nc.events_dropped == 1 && count_rows("/EVENTS.CSV") == 1,
            "gespeichert %lu, verworfen %lu, CSV-Zeilen %d", (unsigned long)g_nc.events_total, (unsigned long)g_nc.events_dropped, count_rows("/EVENTS.CSV"));
      char rel[64]; csv_file(0, rel, sizeof rel);
      CHECK(strstr(rel, ".WAV") != NULL && file_size(rel) > 654 && g_nc.io_errors == 0, "WAV des Tuergeraeusches vorhanden (%ld Bytes), Schreibfehler %lu", file_size(rel), (unsigned long)g_nc.io_errors); }

    printf("T10: Klatschen bei 5 s, jeder SD-Schreibzugriff erzeugt 66 dB(A) Schaltrauschen (60 s Simulation)\n");
    { static burst_t b[1]; b[0] = (burst_t){ 5.0, 0.8, 78.0, 0.10, 0, 0 }; sim_t o = { 60, 0, 0, NULL, 0 };
      g_inject = 1; run_sim("t10", &o, b, 1, B, 33.0, 0.0); g_inject = 0;
      char act[32] = "99"; csv_col(0, 4, act, sizeof act);
      CHECK(g_nc.events_total == 1, "genau 1 Ereignis, keine Selbstausloesung durch Schreibrauschen (erkannt: %lu)", (unsigned long)g_nc.events_total);
      CHECK(atof(act) < 4.0, "Ereignis endet rechtzeitig (aktive Dauer %s s)", act); }

    printf("\n%s (%d Fehler)\n", failures ? "FEHLGESCHLAGEN" : "ALLE GRENZFALLTESTS BESTANDEN", failures);
    return failures != 0;
}
