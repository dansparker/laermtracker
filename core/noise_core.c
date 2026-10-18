#include "noise_core.h"
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979323846

/* ------------------------------------------------------------------ Hilfen */

void nc_civil_from_ms(int64_t ms, int *y, int *mo, int *d, int *h, int *mi, int *s, int *msec)
{
    int64_t secs = ms / 1000, rem = ms % 1000;
    if (rem < 0) { rem += 1000; secs--; }
    int64_t days = secs / 86400, sod = secs % 86400;
    if (sod < 0) { sod += 86400; days--; }
    days += 719468;                                  /* Hinnant: civil_from_days */
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*mo <= 2));
    *h = (int)(sod / 3600); *mi = (int)(sod % 3600 / 60); *s = (int)(sod % 60);
    *msec = (int)rem;
}

int64_t nc_ms_from_civil(int y, int mo, int d, int h, int mi, int s, int msec)
{
    y -= mo <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return ((days * 86400 + h * 3600 + mi * 60 + s) * 1000) + msec;
}

static char s_dec = '.';                                  /* Dezimalzeichen, aus cfg.csv_decimal */

static void fmt1(char *out, size_t sz, float v)         /* "-12.3" ohne Float-printf */
{
    int t = (int)lroundf(v * 10.0f);
    int a = t < 0 ? -t : t;
    snprintf(out, sz, "%s%d%c%d", t < 0 ? "-" : "", a / 10, s_dec, a % 10);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static int io_printf(nc_t *n, void *h, const char *fmt, ...)
{
    char b[256];
    va_list ap; va_start(ap, fmt);
    int len = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (len < 0) return -1;
    if (len >= (int)sizeof b) len = sizeof b - 1;
    return n->io.write(n->io.user, h, b, (uint32_t)len);
}

static void wwrite(nc_t *n, const void *b, uint32_t len)      /* WAV-Schreibzugriff, Fehler werden gezaehlt */
{
    if (n->io.write(n->io.user, n->wav, b, len)) n->io_errors++;
}

/* ------------------------------------------------------------ A-Bewertung */

static void aw_section(nc_biquad_t *q, double fs, int m, double f1, double f2)
{
    /* H(s) = s^m / ((s+p1)(s+p2)), Bilinear mit vorverzerrten Eckfrequenzen */
    double K = 2.0 * fs;
    double p1 = K * tan(PI_F * f1 / fs), p2 = K * tan(PI_F * f2 / fs);
    double d0 = (K + p1) * (K + p2);
    double d1 = (K + p1) * (p2 - K) + (p1 - K) * (K + p2);
    double d2 = (p1 - K) * (p2 - K);
    if (m == 2) { q->b[0] = (float)(K * K / d0); q->b[1] = (float)(-2 * K * K / d0); q->b[2] = (float)(K * K / d0); }
    else        { q->b[0] = (float)(K / d0);     q->b[1] = 0.0f;                     q->b[2] = (float)(-K / d0); }
    q->a[0] = 1.0f; q->a[1] = (float)(d1 / d0); q->a[2] = (float)(d2 / d0);
    q->z1 = q->z2 = 0.0f;
}

static float aw_mag(const nc_biquad_t *s, float f, float fs)
{
    double w = 2.0 * PI_F * f / fs, c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w), m = 1.0;
    for (int i = 0; i < 3; i++) {
        double nr = s[i].b[0] + s[i].b[1] * c1 + s[i].b[2] * c2, ni = -(s[i].b[1] * s1 + s[i].b[2] * s2);
        double dr = 1.0 + s[i].a[1] * c1 + s[i].a[2] * c2,      di = -(s[i].a[1] * s1 + s[i].a[2] * s2);
        m *= sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    return (float)m;
}

static void aw_init(nc_biquad_t *s, uint32_t fs)
{
    const double f1 = 20.598997, f2 = 107.65265, f3 = 737.86223, f4 = 12194.217;
    aw_section(&s[0], fs, 2, f1, f1);
    aw_section(&s[1], fs, 1, f2, f3);
    aw_section(&s[2], fs, 1, f4, f4);
    float g = 1.0f / aw_mag(s, 1000.0f, (float)fs);       /* 0 dB bei 1 kHz */
    for (int k = 0; k < 3; k++) s[0].b[k] *= g;
}

float nc_a_weight_response_db(float f, float fs)
{
    nc_biquad_t s[3];
    aw_init(s, (uint32_t)fs);
    return 20.0f * log10f(aw_mag(s, f, fs));
}

static inline float aw_run(nc_biquad_t *s, float x)
{
    for (int i = 0; i < 3; i++) {                          /* Transponierte Direktform II */
        float y = s[i].b[0] * x + s[i].z1;
        s[i].z1 = s[i].b[1] * x - s[i].a[1] * y + s[i].z2;
        s[i].z2 = s[i].b[2] * x - s[i].a[2] * y;
        x = y;
    }
    return x;
}

/* ----------------------------------------------------------------- Config */

void nc_default_config(nc_config_t *c)
{
    c->fs = 32000;
    c->cal_offset_db = 0.0f;
    c->trig_delta_db = 12.0f;
    c->trig_min_dba = 50.0f;
    c->end_delta_db = 8.0f;
    c->end_hyst_db = 6.0f;
    c->pre_ms = 1000;
    c->post_ms = 3000;
    c->max_event_ms = 60000;
    c->night_start_h = 22;
    c->night_end_h = 6;
    c->lim_night_dba = 42.0f;
    c->lim_day_dba = 55.0f;
    c->wav_hpf_hz = 0.0f;
    c->csv_decimal = '.';
}

void nc_init(nc_t *n, const nc_config_t *c, const nc_io_t *io, int16_t *ring, uint32_t ring_len)
{
    memset(n, 0, sizeof *n);
    n->cfg = *c; n->io = *io; n->ring = ring; n->ring_len = ring_len;
    n->frame_len = c->fs / 100;
    n->M = ring_len * (0xFFFFFFFFu / ring_len);
    double ref_pk = NC_FULLSCALE_24BIT * pow(10.0, NC_MIC_SENS_DBFS / 20.0);
    n->ref_peak = (float)ref_pk;
    n->ref_rms2 = (float)(ref_pk * ref_pk / 2.0);
    aw_init(n->aw, c->fs);
    s_dec = c->csv_decimal == ',' ? ',' : '.';
    if (c->wav_hpf_hz > 0.0f) {                                /* Butterworth-Hochpass 2. Ordnung */
        double w0 = 2.0 * PI_F * c->wav_hpf_hz / c->fs, cw = cos(w0), al = sin(w0) / (2.0 * 0.70710678);
        double a0 = 1.0 + al;
        n->hp_b[0] = (float)((1.0 + cw) / 2.0 / a0); n->hp_b[1] = (float)(-(1.0 + cw) / a0); n->hp_b[2] = n->hp_b[0];
        n->hp_a[0] = (float)(-2.0 * cw / a0); n->hp_a[1] = (float)((1.0 - al) / a0);
    }
    n->ev_max_frames = c->max_event_ms / 10;
    n->ev_post_frames = c->post_ms / 10;
    if (n->ev_max_frames > NC_MAX_PLOT_BINS * 10 - 20) n->ev_max_frames = NC_MAX_PLOT_BINS * 10 - 20;
}

/* ----------------------------------------------------------- Ereignis-I/O */

static uint32_t pdist(const nc_t *n, uint32_t a, uint32_t b) { return b >= a ? b - a : b + (n->M - a); }
static uint32_t psub(const nc_t *n, uint32_t a, uint32_t d)  { return a >= d ? a - d : a + (n->M - d); }

/* Waehrend eines SD-Zugriffs koppelt Schaltrauschen (Klicks, Pegelanhebung) in das Mikrofon ein. Die in dieser Zeit
 * aufgenommenen Frames werden fuer Ausloesung, Ereignisende und Kennwerte ignoriert (die Audiodaten bleiben unveraendert). */
static void blank_add(nc_t *n, uint32_t p0)
{
    uint32_t p1 = n->wr;
    if (p1 == p0) return;
    uint32_t from = p0, to = (uint32_t)(((uint64_t)p1 + n->frame_len) % n->M);     /* Frames, die den Zugriff beruehren, + 1 Frame Nachlauf */
    if (n->blk_n && pdist(n, from, n->blk_to[n->blk_n - 1]) < n->M / 2) { n->blk_to[n->blk_n - 1] = to; return; }   /* ueberlappt: erweitern */
    if (n->blk_n == NC_BLK) { n->blk_to[NC_BLK - 1] = to; return; }
    n->blk_from[n->blk_n] = from; n->blk_to[n->blk_n] = to; n->blk_n++;
}

static int is_blank(nc_t *n, uint32_t pos)
{
    int hit = 0; uint32_t k = 0;
    for (uint32_t i = 0; i < n->blk_n; i++)
        if (pdist(n, n->blk_from[i], pos) <= pdist(n, n->blk_from[i], n->blk_to[i])) hit = 1;
    for (uint32_t i = 0; i < n->blk_n; i++) {                 /* Bereiche vor dieser Position entfernen */
        uint32_t d = pdist(n, n->blk_to[i], pos);
        if (d > 0 && d < n->M / 2) continue;
        n->blk_from[k] = n->blk_from[i]; n->blk_to[k] = n->blk_to[i]; k++;
    }
    n->blk_n = k;
    return hit;
}

static void drain(nc_t *n, int all)
{
    if (!n->wav) return;
    uint32_t wr = n->wr;                                    /* Schnappschuss (ISR schreibt weiter) */
    if (pdist(n, n->rd, wr) > n->ring_len) {                /* Ring ueberlaufen: SD zu langsam */
        uint32_t lost = pdist(n, n->rd, wr) - n->ring_len;
        n->rd = (uint32_t)(((uint64_t)n->rd + lost) % n->M); n->overruns += lost; n->hp_prime = 1;
    }
    if (!all && pdist(n, n->rd, wr) < NC_DRAIN_MIN) return;
    for (;;) {
        uint32_t avail = pdist(n, n->rd, wr);
        if (avail < (all ? 1u : NC_CHUNK_SAMPLES)) break;
        uint32_t cnt = avail < NC_CHUNK_SAMPLES ? avail : NC_CHUNK_SAMPLES;
        uint32_t ri = n->rd % n->ring_len;
        for (uint32_t i = 0; i < cnt; i++) {
            int16_t v = n->ring[ri];
            if (n->cfg.wav_hpf_hz > 0.0f) {                    /* Hochpass nur auf die gespeicherten Daten, Pegelmessung bleibt roh */
                float x = (float)v;
                if (n->hp_prime) { n->hp_x1 = n->hp_x2 = x; n->hp_y1 = n->hp_y2 = 0.0f; n->hp_prime = 0; }
                float y = n->hp_b[0] * x + n->hp_b[1] * n->hp_x1 + n->hp_b[2] * n->hp_x2 - n->hp_a[0] * n->hp_y1 - n->hp_a[1] * n->hp_y2;
                n->hp_x2 = n->hp_x1; n->hp_x1 = x; n->hp_y2 = n->hp_y1; n->hp_y1 = y;
                v = (int16_t)(y > 32767.0f ? 32767 : (y < -32768.0f ? -32768 : (y >= 0 ? y + 0.5f : y - 0.5f)));
            }
            n->chunk[i] = v;
            if (++ri == n->ring_len) ri = 0;
        }
        uint32_t p0 = n->wr;
        wwrite(n, n->chunk, cnt * 2);
        blank_add(n, p0);
        n->wav_data_bytes += cnt * 2;
        n->rd = (uint32_t)(((uint64_t)n->rd + cnt) % n->M);
    }
}

#define WAV_BEXT_DESC   44u
#define WAV_DATA_START  654u

static void wav_begin(nc_t *n, int y, int mo, int d, int h, int mi, int s, int ms)
{
    uint8_t *b = (uint8_t *)n->chunk;
    memset(b, 0, WAV_DATA_START);
    memcpy(b, "RIFF", 4); memcpy(b + 8, "WAVEfmt ", 8);
    put32(b + 16, 16); put16(b + 20, 1); put16(b + 22, 1);
    put32(b + 24, n->cfg.fs); put32(b + 28, n->cfg.fs * 2); put16(b + 32, 2); put16(b + 34, 16);
    memcpy(b + 36, "bext", 4); put32(b + 40, 602);
    snprintf((char *)b + 44, 256, "Laermtracker Ereignis");
    snprintf((char *)b + 300, 32, "laermtracker ICS-43434");
    snprintf((char *)b + 364, 11, "%04d-%02d-%02d", y, mo, d);
    snprintf((char *)b + 374, 9, "%02d:%02d:%02d", h, mi, s);
    uint64_t tr = ((uint64_t)(h * 3600 + mi * 60 + s) * 1000u + (uint64_t)ms) * n->cfg.fs / 1000u;
    put32(b + 382, (uint32_t)tr); put32(b + 386, (uint32_t)(tr >> 32));
    put16(b + 390, 1);
    memcpy(b + 646, "data", 4);
    wwrite(n, b, WAV_DATA_START);
    n->wav_data_bytes = 0;
}

static void wav_label(uint8_t **p, uint32_t id, const char *txt)
{
    uint32_t len = (uint32_t)strlen(txt) + 1, pad = len & 1;
    memcpy(*p, "labl", 4); put32(*p + 4, 4 + len); put32(*p + 8, id);
    memcpy(*p + 12, txt, len); if (pad) (*p)[12 + len] = 0;
    *p += 12 + len + pad;
}

static void tstr(char *out, size_t sz, int64_t ms)
{
    int y, mo, d, h, mi, s, m;
    nc_civil_from_ms(ms, &y, &mo, &d, &h, &mi, &s, &m);
    snprintf(out, sz, "%02d:%02d:%02d.%03d", h, mi, s, m);
}

static void write_svg(nc_t *n, const char *path, float laeq, float lae,
                      int y, int mo, int d, int h, int mi, int s)
{
    void *f = n->io.open(n->io.user, path, 'w');
    if (!f) return;
    float vmin = 1e9f, vmax = -1e9f;
    for (uint32_t i = 0; i < n->plot_n; i++) {
        float v = n->plot[i] / 10.0f;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    float lim = n->ev_limit;
    if (lim < vmax + 30 && lim > vmin - 30) { if (lim < vmin) vmin = lim; if (lim > vmax) vmax = lim; }
    int ymin = (int)floorf(vmin / 10.0f) * 10, ymax = (int)ceilf((vmax + 1.0f) / 10.0f) * 10;
    if (ymin < 0) ymin = 0;
    if (ymax - ymin < 30) ymax = ymin + 30;
    const int X0 = 70, Y0 = 84, PW = 800, PH = 320;
    float T = n->plot_n * 0.1f;
    if (T < 1.0f) T = 1.0f;
    char a[16], b2[16], c[16], e[16], g[16];
    fmt1(a, sizeof a, laeq); fmt1(b2, sizeof b2, n->ev_lafmax); fmt1(c, sizeof c, n->ev_lpeak);
    fmt1(e, sizeof e, lae); fmt1(g, sizeof g, n->ev_bg);

    io_printf(n, f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" "
              "width=\"900\" height=\"460\" viewBox=\"0 0 900 460\" font-family=\"sans-serif\" font-size=\"12\">\n"
              "<rect width=\"900\" height=\"460\" fill=\"#fff\"/>\n");
    io_printf(n, f, "<text x=\"%d\" y=\"26\" font-size=\"18\" font-weight=\"bold\" fill=\"%s\">%sLärmereignis – %04d-%02d-%02d %02d:%02d:%02d</text>\n",
              X0, n->ev_dist ? "#c00" : "#000", n->ev_dist ? "STÖRUNG – " : "", y, mo, d, h, mi, s);
    { char lm[16], ov[16]; fmt1(lm, sizeof lm, lim); fmt1(ov, sizeof ov, n->ev_lafmax - lim);
      io_printf(n, f, "<text x=\"%d\" y=\"64\" fill=\"%s\">Grenzwert %s: LAFmax ≥ %s dB(A) – Messwert %s dB(A), %s dB %s Grenzwert</text>\n",
                X0, n->ev_dist ? "#c00" : "#2a7a2a", n->ev_night ? "nachts" : "tagsüber", lm, b2, ov, n->ev_dist ? "über" : "unter"); }
    io_printf(n, f, "<text x=\"%d\" y=\"46\">LAeq = %s dB(A) · LAFmax = %s dB(A) · Lpeak = %s dB (unbewertet) · "
              "LAE = %s dB(A) · Hintergrund = %s dB(A)</text>\n", X0, a, b2, c, e, g);
    for (int v = ymin; v <= ymax; v += 10) {
        int yy = Y0 + PH - (PH * (v - ymin)) / (ymax - ymin);
        io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#ddd\"/>"
                  "<text x=\"%d\" y=\"%d\" text-anchor=\"end\">%d</text>\n", X0, yy, X0 + PW, yy, X0 - 6, yy + 4, v);
    }
    float step = T <= 12 ? 1.0f : (T <= 60 ? 5.0f : 10.0f);
    for (float t = 0; t <= T + 0.001f; t += step) {
        int xx = X0 + (int)(PW * t / T);
        io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#eee\"/>"
                  "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\">%d</text>\n", xx, Y0, xx, Y0 + PH, xx, Y0 + PH + 16, (int)(t + 0.5f));
    }
    io_printf(n, f, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"none\" stroke=\"#444\"/>\n", X0, Y0, PW, PH);
    io_printf(n, f, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\">Zeit seit Dateianfang [s]</text>\n", X0 + PW / 2, Y0 + PH + 38);
    io_printf(n, f, "<text transform=\"translate(18,%d) rotate(-90)\" text-anchor=\"middle\">LAF (Fast, 125 ms), Max je 100 ms [dB(A) SPL]</text>\n", Y0 + PH / 2);
    /* Ausloesung + LAeq */
    int xt = X0 + (int)(PW * (n->ev_trig_frame * 0.01f) / T);
    io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#888\" stroke-dasharray=\"4 3\"/>"
              "<text x=\"%d\" y=\"%d\" fill=\"#666\">Auslösung</text>\n", xt, Y0, xt, Y0 + PH, xt + 4, Y0 + 12);
    float vl = laeq < ymin ? ymin : (laeq > ymax ? ymax : laeq);
    int yl = Y0 + PH - (int)(PH * (vl - ymin) / (ymax - ymin));
    io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#d55\" stroke-dasharray=\"6 3\"/>"
              "<text x=\"%d\" y=\"%d\" text-anchor=\"end\" fill=\"#d55\">LAeq %s</text>\n", X0, yl, X0 + PW, yl, X0 + PW - 4, yl - 4, a);
    if (lim >= ymin && lim <= ymax) {
        int yg = Y0 + PH - (int)(PH * (lim - ymin) / (ymax - ymin));
        io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#e08a00\" stroke-width=\"2\" stroke-dasharray=\"2 4\"/>"
                  "<text x=\"%d\" y=\"%d\" fill=\"#e08a00\">Grenzwert</text>\n", X0, yg, X0 + PW, yg, X0 + 4, yg - 4);
    }
    /* Kurve */
    io_printf(n, f, "<polyline fill=\"none\" stroke=\"#1769aa\" stroke-width=\"1.5\" points=\"");
    for (uint32_t i = 0; i < n->plot_n; i++) {
        int xx = X0 + (int)(PW * ((i + 0.5f) * 0.1f) / T);
        int yy = Y0 + PH - (int)(PH * (n->plot[i] / 10.0f - ymin) / (ymax - ymin));
        io_printf(n, f, "%d,%d ", xx, yy);
    }
    io_printf(n, f, "\"/>\n");
    int xm = X0 + (int)(PW * (n->ev_lafmax_frame * 0.01f) / T);
    int ym = Y0 + PH - (int)(PH * (n->ev_lafmax - ymin) / (ymax - ymin));
    io_printf(n, f, "<circle cx=\"%d\" cy=\"%d\" r=\"4\" fill=\"#c00\"/><text x=\"%d\" y=\"%d\" fill=\"#c00\">LAFmax %s</text>\n",
              xm, ym, xm + 8, ym + 4, b2);
    io_printf(n, f, "</svg>\n");
    n->io.close(n->io.user, f);
}


/* ------------------------------------------------- CSV + Tagesdiagramm */

#define CSV_HEAD "datum;zeit;zeitraum;aktiv_s;LAeq_dBA;LAFmax_dBA;Lpeak_dB;LAE_dBA;hintergrund_dBA;grenzwert_dBA;ueber_dB;stoerung;datei;ueberlaeufe;schreibfehler\n"

static void csv_append(nc_t *n, const char *path, const char *row)
{
    void *f = n->io.open(n->io.user, path, 'r');            /* Existenz pruefen */
    int fresh = (f == NULL);
    if (f) n->io.close(n->io.user, f);
    f = n->io.open(n->io.user, path, 'a');
    if (!f) return;
    if (fresh) n->io.write(n->io.user, f, CSV_HEAD, sizeof CSV_HEAD - 1);
    n->io.write(n->io.user, f, row, (uint32_t)strlen(row));
    n->io.close(n->io.user, f);
}

static int parse10(const char *p)                           /* "-12.3" -> -123 */
{
    int neg = 0, v = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    v *= 10;
    if ((*p == '.' || *p == ',') && p[1] >= '0' && p[1] <= '9') v += p[1] - '0';
    return neg ? -v : v;
}

/* Alte Dateien haben an Spalte 3 die Art (TUER/KNALL/SONST); neue nicht. Rueckgabe: Spaltenversatz fuer alle folgenden Spalten. */
static int csv_off(char *const fld[]) { return (fld[3][0] >= 'A' && fld[3][0] <= 'Z') ? 1 : 0; }

static void day_parse_line(nc_t *n, char *line)
{
    char *fld[16]; int nf = 0;
    fld[nf++] = line;
    for (char *p = line; *p && nf < 16; p++) if (*p == ';') { *p = 0; fld[nf++] = p + 1; }
    if (nf < 5 || line[0] < '0' || line[0] > '9' || n->day_n >= NC_DAY_MAX || strlen(fld[1]) < 8) return;
    int o = csv_off(fld);
    if (nf < 12 + o) return;
    nc_dayev_t *e = &n->day[n->day_n++];
    const char *t = fld[1];
    e->sod = (uint32_t)(((t[0] - '0') * 10 + (t[1] - '0')) * 3600 + ((t[3] - '0') * 10 + (t[4] - '0')) * 60 + (t[6] - '0') * 10 + (t[7] - '0'));
    e->lafmax10 = (int16_t)parse10(fld[5 + o]);
    e->bg10 = (int16_t)parse10(fld[8 + o]);
    e->dist = fld[11 + o][0] == '1';
}

static void write_day_svg(nc_t *n)
{
    char path[32];
    snprintf(path, sizeof path, "/%s/TAG.CSV", n->dir);
    void *f = n->io.open(n->io.user, path, 'r');
    if (!f) return;
    n->day_n = 0;
    char line[200]; uint32_t ll = 0; int32_t got;
    while ((got = n->io.read(n->io.user, f, n->chunk, sizeof n->chunk)) > 0) {
        const char *b = (const char *)n->chunk;
        for (int32_t i = 0; i < got; i++) {
            if (b[i] == '\n' || b[i] == '\r') { if (ll) { line[ll] = 0; day_parse_line(n, line); ll = 0; } }
            else if (ll < sizeof line - 1) line[ll++] = b[i];
        }
    }
    n->io.close(n->io.user, f);

    const int ns = n->cfg.night_start_h, ne = n->cfg.night_end_h;
    float ln = n->cfg.lim_night_dba, ld = n->cfg.lim_day_dba;
    float lo = ln < ld ? ln : ld, hi = ln > ld ? ln : ld;
    uint32_t ndn = 0, ndd = 0;
    for (uint32_t i = 0; i < n->day_n; i++) {
        const nc_dayev_t *e = &n->day[i];
        float bg = e->bg10 / 10.0f, v = e->lafmax10 / 10.0f;
        if (bg < lo) lo = bg;
        if (v > hi) hi = v;
        uint32_t hr = e->sod / 3600;
        int night = ns > ne ? (hr >= (uint32_t)ns || hr < (uint32_t)ne) : (hr >= (uint32_t)ns && hr < (uint32_t)ne);
        if (e->dist) { if (night) ndn++; else ndd++; }
    }
    int ymin = (int)floorf((lo - 5.0f) / 10.0f) * 10, ymax = (int)ceilf((hi + 2.0f) / 10.0f) * 10;
    if (ymin < 0) ymin = 0;
    if (ymax - ymin < 30) ymax = ymin + 30;
    const int X0 = 70, Y0 = 84, PW = 1880, PH = 300;           /* 78 Pixel je Stunde */
    #define DX(sec) (X0 + (int)((float)PW * (float)(sec) / 86400.0f))
    #define DY(v)   (Y0 + PH - (int)((float)PH * ((v) - (float)ymin) / (float)(ymax - ymin)))
    int nseg[2][2], dseg[2][2], nn = 0, nd = 0;
    if (ns > ne) { nseg[nn][0] = 0; nseg[nn++][1] = ne; nseg[nn][0] = ns; nseg[nn++][1] = 24; dseg[nd][0] = ne; dseg[nd++][1] = ns; }
    else         { nseg[nn][0] = ns; nseg[nn++][1] = ne; dseg[nd][0] = 0; dseg[nd++][1] = ns; dseg[nd][0] = ne; dseg[nd++][1] = 24; }

    snprintf(path, sizeof path, "/%s/TAG.SVG", n->dir);
    f = n->io.open(n->io.user, path, 'w');
    if (!f) return;
    char lns[16], lds[16];
    fmt1(lns, sizeof lns, ln); fmt1(lds, sizeof lds, ld);
    io_printf(n, f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2000\" height=\"470\" "
              "viewBox=\"0 0 2000 470\" font-family=\"sans-serif\" font-size=\"12\">\n<rect width=\"2000\" height=\"470\" fill=\"#fff\"/>\n");
    io_printf(n, f, "<text x=\"%d\" y=\"26\" font-size=\"18\" font-weight=\"bold\">Tagesverlauf 20%.2s-%.2s-%.2s – %lu Ereignisse, "
              "<tspan fill=\"#c00\">%lu Störungen</tspan> (nachts %lu, tagsüber %lu)</text>\n", X0, n->dir, n->dir + 2, n->dir + 4,
              (unsigned long)n->day_n, (unsigned long)(ndn + ndd), (unsigned long)ndn, (unsigned long)ndd);
    io_printf(n, f, "<text x=\"%d\" y=\"46\">Markerhöhe = LAFmax, Fuß = Hintergrundpegel · rot = Störung (LAFmax ≥ Grenzwert: nachts %s, tagsüber %s dB(A)) "
              "· jeder Punkt = ein Ereignis%s</text>\n", X0, lns, lds, n->day_n >= NC_DAY_MAX ? " · nur die ersten Ereignisse dargestellt" : "");
    for (int i = 0; i < nn; i++)
        io_printf(n, f, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"#e6ebf4\"/>\n", DX(nseg[i][0] * 3600), Y0,
                  DX(nseg[i][1] * 3600) - DX(nseg[i][0] * 3600), PH);
    for (int v = ymin; v <= ymax; v += 10)
        io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#ddd\"/><text x=\"%d\" y=\"%d\" text-anchor=\"end\">%d</text>\n",
                  X0, DY((float)v), X0 + PW, DY((float)v), X0 - 6, DY((float)v) + 4, v);
    for (int hr = 0; hr < 24; hr++)                                         /* halbe Stunden: feine Hilfslinien */
        io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#f2f2f2\"/>\n", DX(hr * 3600 + 1800), Y0, DX(hr * 3600 + 1800), Y0 + PH);
    for (int hr = 0; hr <= 24; hr++)                                        /* jede volle Stunde beschriften */
        io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#eee\"/><text x=\"%d\" y=\"%d\" text-anchor=\"middle\">%02d:00</text>\n",
                  DX(hr * 3600), Y0, DX(hr * 3600), Y0 + PH, DX(hr * 3600), Y0 + PH + 16, hr);
    io_printf(n, f, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"none\" stroke=\"#444\"/>\n", X0, Y0, PW, PH);
    io_printf(n, f, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\">Uhrzeit (lokal) – blau hinterlegt: Nachtzeit</text>\n", X0 + PW / 2, Y0 + PH + 38);
    io_printf(n, f, "<text transform=\"translate(18,%d) rotate(-90)\" text-anchor=\"middle\">LAFmax der Ereignisse [dB(A) SPL]</text>\n", Y0 + PH / 2);
    for (int i = 0; i < nn; i++)                                   /* Grenzwert Nacht */
        io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#e08a00\" stroke-width=\"2\" stroke-dasharray=\"2 4\"/>\n",
                  DX(nseg[i][0] * 3600), DY(ln), DX(nseg[i][1] * 3600), DY(ln));
    for (int i = 0; i < nd; i++)                                   /* Grenzwert Tag */
        io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#e08a00\" stroke-width=\"2\" stroke-dasharray=\"2 4\"/>\n",
                  DX(dseg[i][0] * 3600), DY(ld), DX(dseg[i][1] * 3600), DY(ld));
    for (uint32_t i = 0; i < n->day_n; i++) {
        const nc_dayev_t *e = &n->day[i];
        float bgv = e->bg10 / 10.0f, lv = e->lafmax10 / 10.0f;         /* Werte unter dem Diagrammrand am Rand abschneiden (CSV behaelt den echten Wert) */
        if (bgv < (float)ymin) bgv = (float)ymin;
        if (lv < (float)ymin) lv = (float)ymin;
        int cx = DX(e->sod), yb = DY(bgv), yv = DY(lv);
        const char *col = e->dist ? "#d32f2f" : "#1769aa";
        io_printf(n, f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"%s\" stroke-opacity=\"0.35\" stroke-width=\"2\"/>\n", cx, yb, cx, yv, col);
        io_printf(n, f, "<circle cx=\"%d\" cy=\"%d\" r=\"4.5\" fill=\"%s\"/>\n", cx, yv, col);
    }
    io_printf(n, f, "</svg>\n");
    n->io.close(n->io.user, f);
    #undef DX
    #undef DY
}

static void finish_event_inner(nc_t *n)
{
    drain(n, 1);
    /* Zeitmarken (cue + labl) hinter die Audiodaten */
    uint8_t *b = (uint8_t *)n->chunk, *p = b;
    uint32_t off[3] = { n->ev_trig_frame * n->frame_len, n->ev_lafmax_frame * n->frame_len,
                        (n->ev_last_L + 1) * n->frame_len };
    int64_t tm[3] = { n->ev_start_ms + (int64_t)off[0] * 1000 / n->cfg.fs,
                      n->ev_start_ms + (int64_t)off[1] * 1000 / n->cfg.fs,
                      n->ev_start_ms + (int64_t)off[2] * 1000 / n->cfg.fs };
    const char *nm[3] = { "Ausloesung", "LAFmax", "Ende Aktivitaet" };
    if (n->wav_data_bytes & 1) *p++ = 0;
    memcpy(p, "cue ", 4); put32(p + 4, 4 + 24 * 3); put32(p + 8, 3); p += 12;
    for (uint32_t i = 0; i < 3; i++, p += 24) {
        put32(p, i + 1); put32(p + 4, i); memcpy(p + 8, "data", 4);
        put32(p + 12, 0); put32(p + 16, 0); put32(p + 20, off[i]);
    }
    uint8_t *list = p; memcpy(p, "LIST", 4); p += 8; memcpy(p, "adtl", 4); p += 4;
    for (uint32_t i = 0; i < 3; i++) {
        char t[64], ts[16]; tstr(ts, sizeof ts, tm[i]);
        snprintf(t, sizeof t, "%s %s", nm[i], ts);
        wav_label(&p, i + 1, t);
    }
    put32(list + 4, (uint32_t)(p - list - 8));
    wwrite(n, b, (uint32_t)(p - b));

    /* Kennwerte */
    uint32_t cn = n->ev_n_snap ? n->ev_n_snap : 1;
    float laeq = 10.0f * log10f((float)(n->ev_sumA_snap / cn) / n->ref_rms2) + NC_REF_SPL_DB + n->cfg.cal_offset_db;
    float lae = laeq + 10.0f * log10f(cn * 0.01f);
    char a[16], bb[16], c[16], desc[256];
    fmt1(a, sizeof a, laeq); fmt1(bb, sizeof bb, n->ev_lafmax); fmt1(c, sizeof c, n->ev_lpeak);
    snprintf(desc, sizeof desc, "Laermereignis LAeq=%s dB(A) LAFmax=%s dB(A) Lpeak=%s dB", a, bb, c);
    uint32_t total = WAV_DATA_START + n->wav_data_bytes + (uint32_t)(p - b);
    uint8_t hdr[4];
    n->io.seek(n->io.user, n->wav, 4);   put32(hdr, total - 8);              wwrite(n, hdr, 4);
    n->io.seek(n->io.user, n->wav, 650); put32(hdr, n->wav_data_bytes);       wwrite(n, hdr, 4);
    uint8_t dz[256]; memset(dz, 0, sizeof dz); memcpy(dz, desc, strlen(desc));
    n->io.seek(n->io.user, n->wav, WAV_BEXT_DESC); wwrite(n, dz, sizeof dz);
    n->io.close(n->io.user, n->wav);
    n->wav = NULL;

    if (n->bin_cnt) { if (n->plot_n < NC_MAX_PLOT_BINS) n->plot[n->plot_n++] = n->bin_max; n->bin_cnt = 0; }

    int ty, tmo, td, th, tmi, ts, tms;                    /* Ausloesezeit */
    nc_civil_from_ms(tm[0], &ty, &tmo, &td, &th, &tmi, &ts, &tms);
    const int ns = n->cfg.night_start_h, ne = n->cfg.night_end_h;
    int night = ns > ne ? (th >= ns || th < ne) : (th >= ns && th < ne);
    float lim = night ? n->cfg.lim_night_dba : n->cfg.lim_day_dba;
    int dist = n->ev_lafmax >= lim;
    n->ev_limit = lim; n->ev_dist = (uint8_t)dist; n->ev_night = (uint8_t)night;

    char path[32];
    snprintf(path, sizeof path, "/%s/%s.SVG", n->dir, n->base);
    write_svg(n, path, laeq, lae, ty, tmo, td, th, tmi, ts);

    char dur[16], lp[16], bgs[16], lm[16], ov[16], row[256];
    uint32_t cs = n->ev_last_L + 1 - n->ev_trig_frame;   /* aktive Dauer in 10-ms-Frames */
    snprintf(dur, sizeof dur, "%lu%c%02lu", (unsigned long)(cs / 100), s_dec, (unsigned long)(cs % 100));
    fmt1(lp, sizeof lp, lae); fmt1(bgs, sizeof bgs, n->ev_bg);
    fmt1(lm, sizeof lm, lim); fmt1(ov, sizeof ov, n->ev_lafmax - lim);
    snprintf(row, sizeof row, "%04d-%02d-%02d;%02d:%02d:%02d.%03d;%s;%s;%s;%s;%s;%s;%s;%s;%s;%d;/%s/%s.WAV;%lu;%lu\n",
             ty, tmo, td, th, tmi, ts, tms, night ? "NACHT" : "TAG", dur, a, bb, c, lp, bgs, lm, ov, dist,
             n->dir, n->base, (unsigned long)n->overruns, (unsigned long)n->io_errors);
    csv_append(n, "/EVENTS.CSV", row);
    snprintf(path, sizeof path, "/%s/TAG.CSV", n->dir);
    csv_append(n, path, row);
    write_day_svg(n);
    n->events_total++;
    n->state = NC_IDLE;
}

/* ------------------------------------------------------- Frame-Auswertung */

static void plot_add(nc_t *n, int16_t v)
{
    if (v > n->bin_max || n->bin_cnt == 0) n->bin_max = v;
    if (++n->bin_cnt == 10) {
        if (n->plot_n < NC_MAX_PLOT_BINS) n->plot[n->plot_n++] = n->bin_max;
        n->bin_cnt = 0;
    }
}

static float end_threshold(const nc_t *n)
{
    float a = n->bg_db + n->cfg.end_delta_db, b = n->cfg.trig_min_dba - n->cfg.end_hyst_db;
    return a > b ? a : b;
}

static void event_account(nc_t *n, float L, float laf, float laf_act, float lpk, double meansq, uint32_t idx, int blank)
{
    n->ev_sumA += meansq; n->ev_n++;
    float thr = end_threshold(n);
    if (!blank) {                                             /* gesperrte Frames (SD-Schaltrauschen) zaehlen nicht als Aktivitaet */
        if (laf_act >= thr) n->ev_last_active = idx;          /* Ende-Timer: LAF (125 ms) statt 10-ms-Pegel (Zufallsspitzen) */
        if (L >= thr) {                                       /* Dauer/LAeq: tatsaechliche Schallenergie, ohne LAF-Nachklingen */
            n->ev_last_L = idx;
            n->ev_sumA_snap = n->ev_sumA; n->ev_n_snap = n->ev_n;
        }
    }
    if (laf > n->ev_lafmax) { n->ev_lafmax = laf; n->ev_lafmax_frame = idx; }
    if (lpk > n->ev_lpeak) n->ev_lpeak = lpk;
}

static void start_event_inner(nc_t *n, float L, float laf, float lpk, double meansq, uint32_t endpos)
{
    uint32_t ring_frames = n->ring_len / n->frame_len;
    uint32_t pre = n->cfg.pre_ms / 10;
    if (pre > n->frames_seen - 1) pre = n->frames_seen - 1;
    if (pre > n->hist_n - 1) pre = n->hist_n - 1;
    uint32_t pre_max = ring_frames > 50 ? ring_frames - 50 : ring_frames / 2;   /* 0.5 s Reserve fuer SD-Wartezeiten */
    if (pre > pre_max) pre = pre_max;
    pre = pre / 10 * 10;
    n->ev_pre_frames = pre;
    n->rd = psub(n, endpos, n->frame_len * (pre + 1));
    int64_t now = n->io.now_ms(n->io.user);                /* entspricht der Schreibposition n->wr */
    n->ev_start_ms = now - (int64_t)pdist(n, n->rd, n->wr) * 1000 / n->cfg.fs;
    int y, mo, d, h, mi, s, ms;
    nc_civil_from_ms(n->ev_start_ms, &y, &mo, &d, &h, &mi, &s, &ms);
    snprintf(n->dir, sizeof n->dir, "%02d%02d%02d", y % 100, mo, d);
    snprintf(n->base, sizeof n->base, "%02d%02d%02d", h, mi, s);
    char path[32];
    snprintf(path, sizeof path, "/%s", n->dir);
    n->io.mkdir(n->io.user, path);
    snprintf(path, sizeof path, "/%s/%s.WAV", n->dir, n->base);
    n->wav = n->io.open(n->io.user, path, 'w');
    if (!n->wav) { n->holdoff = 300; return; }             /* SD-Fehler: verwerfen, 3 s Pause */
    wav_begin(n, y, mo, d, h, mi, s, ms);
    n->hp_prime = 1;

    n->plot_n = 0; n->bin_cnt = 0;
    for (uint32_t i = 0; i <= pre; i++)                    /* Vorlauf aus der Historie + Ausloeseframe */
        plot_add(n, (int16_t)n->hist[(n->hist_n - 1 - pre + i) % NC_HIST_FRAMES]);
    n->state = NC_ACTIVE;
    n->ev_frames = pre + 1;
    n->ev_trig_frame = pre;
    n->ev_last_active = pre; n->ev_last_L = pre;
    n->ev_sumA = 0; n->ev_n = 0; n->ev_sumA_snap = 0; n->ev_n_snap = 0;
    n->ev_lafmax = -999; n->ev_lpeak = -999;
    n->ev_bg = n->bg_db;
    event_account(n, L, laf, laf, lpk, meansq, pre, 0);
}

static void start_event(nc_t *n, float L, float laf, float lpk, double meansq, uint32_t endpos)
{
    uint32_t p0 = n->wr;
    start_event_inner(n, L, laf, lpk, meansq, endpos);
    blank_add(n, p0);
}

static void finish_event(nc_t *n)
{
    uint32_t p0 = n->wr;
    finish_event_inner(n);
    blank_add(n, p0);
}

static void frame_consume(nc_t *n, const nc_frame_t *fr)
{
    int blank = is_blank(n, fr->pos);
    double meansq = fr->meansq;
    if (meansq < 1e-3) meansq = 1e-3;
    float cal = NC_REF_SPL_DB + n->cfg.cal_offset_db;
    float L = 10.0f * log10f((float)meansq / n->ref_rms2) + cal;
    if (L < 0.0f) L = 0.0f;                                    /* Messbereichsuntergrenze (digitale Stille) */
    float pk = fr->peak < 1 ? 1.0f : fr->peak;
    float lpk = 20.0f * log10f(pk / n->ref_peak) + cal;
    if (lpk < 0.0f) lpk = 0.0f;

    const float alpha = 1.0f - expf(-0.010f / 0.125f);     /* Fast: tau = 125 ms */
    if (n->frames_seen == 0) n->laf_energy = n->lafc_energy = (float)meansq;
    else { n->laf_energy += alpha * ((float)meansq - n->laf_energy); if (!blank) n->lafc_energy += alpha * ((float)meansq - n->lafc_energy); }
    float laf = 10.0f * log10f(n->laf_energy / n->ref_rms2) + cal;
    if (laf < 0.0f) laf = 0.0f;
    float laf_act = 10.0f * log10f(n->lafc_energy / n->ref_rms2) + cal;
    if (laf_act < 0.0f) laf_act = 0.0f;
    float lafc = laf < 0 ? 0 : laf;
    n->hist[n->hist_n % NC_HIST_FRAMES] = (uint16_t)(lafc * 10.0f + 0.5f);
    n->hist_n++;
    n->frames_seen++;

    if (n->state == NC_IDLE) {
        float thr = n->bg_db + n->cfg.trig_delta_db;
        if (thr < n->cfg.trig_min_dba) thr = n->cfg.trig_min_dba;
        if (n->frames_seen <= 100) {
            if (!blank || n->frames_seen == 1) n->bg_db = n->frames_seen == 1 ? L : n->bg_db + (L - n->bg_db) * 0.05f;
        } else if (blank) {
            /* SD-Zugriff: Frame ignorieren */
        } else if (n->holdoff) {
            n->holdoff--;
        } else if (L >= thr) {
            start_event(n, L, laf, lpk, meansq, fr->pos);
        } else if (L < n->bg_db) {
            n->bg_db += (L - n->bg_db) * 0.05f;
        } else if (L < n->bg_db + 6.0f) {
            n->bg_db += (L - n->bg_db) * 0.0015f;
        }
    } else {
        uint32_t idx = n->ev_frames++;
        plot_add(n, (int16_t)n->hist[(n->hist_n - 1) % NC_HIST_FRAMES]);
        event_account(n, L, laf, laf_act, lpk, meansq, idx, blank);
        drain(n, 0);
        if (idx - n->ev_last_active >= n->ev_post_frames || n->ev_frames >= n->ev_max_frames)
            finish_event(n);
    }
    if (!blank) n->prev_db = L;
}

void nc_push24(nc_t *n, const int32_t *s, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        int32_t x = s[i];
        n->ring[n->widx] = (int16_t)(x >> 8);
        if (++n->widx == n->ring_len) n->widx = 0;
        uint32_t w = n->wr + 1; if (w == n->M) w = 0; n->wr = w;
        float y = aw_run(n->aw, (float)x);
        n->frame_sumA += y * y;
        float ax = x < 0 ? (float)-x : (float)x;
        if (ax > n->frame_peak) n->frame_peak = ax;
        if (++n->frame_pos == n->frame_len) {
            uint32_t h = n->fq_head;
            if (h - n->fq_tail >= NC_FQ_LEN) n->fq_dropped++;
            else {
                nc_frame_t *f = &n->fq[h % NC_FQ_LEN];
                f->meansq = n->frame_sumA / (float)n->frame_len; f->peak = n->frame_peak; f->pos = w;
                n->fq_head = h + 1;
            }
            n->frame_sumA = 0; n->frame_peak = 0; n->frame_pos = 0;
        }
    }
}

void nc_poll(nc_t *n)
{
    while (n->fq_tail != n->fq_head) {
        nc_frame_t f = n->fq[n->fq_tail % NC_FQ_LEN];
        n->fq_tail++;
        frame_consume(n, &f);
    }
}

void nc_process(nc_t *n, const int32_t *s, uint32_t count)
{
    nc_push24(n, s, count);
    nc_poll(n);
}

void nc_flush(nc_t *n)
{
    if (n->state == NC_ACTIVE && n->wav) finish_event(n);
}

/* ------------------------------------------------------------------------------------------------------------------
 * Nachtraegliche Pegelkorrektur: rechnet vorhandene Ergebnisse (EVENTS.CSV, TAG.CSV, Ereignis- und Tagesplots) um delta_db um.
 * Alle Pegel (LAeq, LAFmax, Lpeak, LAE, Hintergrund) sind linear in der Kalibrierung, verschieben sich also genau um delta_db.
 * Neu bewertet werden daraus abgeleitete Angaben: Ueberschreitung/Stoerung gegen den Grenzwert. Die Ereigniskurven werden aus den WAV-Dateien neu berechnet und auf
 * den neuen LAFmax ausgerichtet. Das Original bleibt als /EVENTS_ALT.CSV erhalten; die Aktion wird in /KORREKTUR.LOG vermerkt.
 * ------------------------------------------------------------------------------------------------------------------ */

static int copy_file(nc_t *n, const char *src, const char *dst)
{
    void *a = n->io.open(n->io.user, src, 'r');
    if (!a) return -1;
    void *b = n->io.open(n->io.user, dst, 'w');
    if (!b) { n->io.close(n->io.user, a); return -1; }
    int32_t got; int rc = 0;
    while ((got = n->io.read(n->io.user, a, n->chunk, sizeof n->chunk)) > 0)
        if (n->io.write(n->io.user, b, n->chunk, (uint32_t)got)) rc = -1;
    n->io.close(n->io.user, a); n->io.close(n->io.user, b);
    return rc;
}

#define RR_MAX_FRAMES 7400

/* Ereigniskurve (LAF) aus der WAV-Datei neu berechnen und den Plot schreiben */
static int rerender_event(nc_t *n, const char *wavp, const char *svgp, float laeq, float lae,
                          int y, int mo, int d, int h, int mi, int s)
{
    static int16_t lafs[RR_MAX_FRAMES];                       /* LAF je 10-ms-Frame in 0.1 dB (ohne Kalibrierung) */
    void *f = n->io.open(n->io.user, wavp, 'r');
    if (!f) return -1;
    uint8_t hd[654];
    if (n->io.read(n->io.user, f, hd, 654) != 654 || memcmp(hd, "RIFF", 4)) { n->io.close(n->io.user, f); return -1; }
    uint32_t rate = hd[24] | hd[25] << 8 | hd[26] << 16 | (uint32_t)hd[27] << 24;
    uint32_t dbytes = hd[650] | hd[651] << 8 | hd[652] << 16 | (uint32_t)hd[653] << 24;
    if (rate < 8000 || rate % 100 || dbytes < 2) { n->io.close(n->io.user, f); return -1; }
    uint32_t fl = rate / 100;
    nc_biquad_t aw[3]; aw_init(aw, rate);
    int16_t sb[1024]; uint32_t remaining = dbytes / 2, nfr = 0, fp = 0, primed = 0;
    float sum = 0, e = 0; const float alpha = 1.0f - expf(-0.010f / 0.125f);
    while (remaining) {
        uint32_t want = remaining < 1024 ? remaining : 1024;
        int32_t got = n->io.read(n->io.user, f, sb, want * 2) / 2;
        if (got <= 0) break;
        if (!primed) { primed = 1; for (uint32_t i = 0; i < rate * 3 / 10; i++) aw_run(aw, (float)sb[0] * 256.0f); }   /* Einschwingen */
        for (int32_t i = 0; i < got; i++) {
            float yv = aw_run(aw, (float)sb[i] * 256.0f);
            sum += yv * yv;
            if (++fp == fl) {
                float ms = sum / (float)fl; if (ms < 1e-3f) ms = 1e-3f;
                e = nfr == 0 ? ms : e + alpha * (ms - e);
                if (nfr < RR_MAX_FRAMES) lafs[nfr++] = (int16_t)lroundf((10.0f * log10f(e / n->ref_rms2) + NC_REF_SPL_DB) * 10.0f);
                sum = 0; fp = 0;
            }
        }
        remaining -= (uint32_t)got;
    }
    /* Zeitmarken (cue-Chunk hinter den Daten): Ausloesung und LAFmax */
    uint32_t trig = 100, mx = 0xFFFFFFFFu;
    if (n->io.seek(n->io.user, f, 654 + dbytes + (dbytes & 1)) == 0) {
        uint8_t cu[12 + 24 * 3];
        if (n->io.read(n->io.user, f, cu, sizeof cu) == (int32_t)sizeof cu && !memcmp(cu, "cue ", 4)) {
            trig = (cu[12 + 20] | cu[12 + 21] << 8 | cu[12 + 22] << 16 | (uint32_t)cu[12 + 23] << 24) / fl;
            mx = (cu[36 + 20] | cu[36 + 21] << 8 | cu[36 + 22] << 16 | (uint32_t)cu[36 + 23] << 24) / fl;
        }
    }
    n->io.close(n->io.user, f);
    if (nfr == 0) return -1;
    if (trig >= nfr) trig = 0;
    int16_t best = -32768; uint32_t bi = trig;
    for (uint32_t i = trig; i < nfr; i++) if (lafs[i] > best) { best = lafs[i]; bi = i; }
    if (mx >= nfr) mx = bi;
    int16_t align = (int16_t)lroundf(n->ev_lafmax * 10.0f) - best;     /* Kurve auf den neuen LAFmax der CSV ausrichten */
    n->plot_n = 0;
    for (uint32_t i = 0; i < nfr && n->plot_n < NC_MAX_PLOT_BINS; i += 10) {
        int16_t m = -32768;
        for (uint32_t k = i; k < i + 10 && k < nfr; k++) if (lafs[k] > m) m = lafs[k];
        int v = m + align; n->plot[n->plot_n++] = (int16_t)(v < 0 ? 0 : v);
    }
    n->ev_trig_frame = trig; n->ev_lafmax_frame = mx;
    write_svg(n, svgp, laeq, lae, y, mo, d, h, mi, s);
    return 0;
}

static void recal_line(nc_t *n, const char *line, float delta, nc_recal_t *res, char days[][8], uint32_t *nd)
{
    if (line[0] < '0' || line[0] > '9') return;                 /* Kopfzeile / Muell */
    char buf[512]; snprintf(buf, sizeof buf, "%s", line);
    char *fld[17]; int nf = 0; fld[nf++] = buf;
    for (char *p = buf; *p && nf < 17; p++) if (*p == ';') { *p = 0; fld[nf++] = p + 1; }
    if (nf < 5) { res->skipped++; return; }
    int o = csv_off(fld);                                       /* 1 bei alten Dateien mit Spalte "typ" (wird entfernt) */
    if (nf < 14 + o || strlen(fld[12 + o]) < 12) { res->skipped++; return; }
    float laeq = parse10(fld[4 + o]) / 10.0f + delta, lafmax = parse10(fld[5 + o]) / 10.0f + delta, lpk = parse10(fld[6 + o]) / 10.0f + delta;
    float lae = parse10(fld[7 + o]) / 10.0f + delta, bg = parse10(fld[8 + o]) / 10.0f + delta, lim = parse10(fld[9 + o]) / 10.0f;
    if (bg < 0.0f) bg = 0.0f;                                   /* alte Dateien: Pegel unter 0 dB (digitale Stille) als 0 ausweisen */
    float over = lafmax - lim; int dist = over >= 0.0f;
    char a[16], b[16], c[16], dd[16], e[16], ov[16], row[400];
    fmt1(a, sizeof a, laeq); fmt1(b, sizeof b, lafmax); fmt1(c, sizeof c, lpk); fmt1(dd, sizeof dd, lae); fmt1(e, sizeof e, bg); fmt1(ov, sizeof ov, over);
    snprintf(row, sizeof row, "%s;%s;%s;%s;%s;%s;%s;%s;%s;%s;%s;%d;%s;%s;%s\n", fld[0], fld[1], fld[2], fld[3 + o], a, b, c, dd, e, fld[9 + o], ov,
             dist, fld[12 + o], fld[13 + o], nf > 14 + o ? fld[14 + o] : "0");
    char day[8] = {0}; snprintf(day, sizeof day, "%.6s", fld[12 + o] + 1);
    int seen = 0;
    for (uint32_t i = 0; i < *nd; i++) if (!strcmp(days[i], day)) seen = 1;
    char tagnew[32]; snprintf(tagnew, sizeof tagnew, "/%.6s/TAG.NEU", day);
    if (!seen) { if (*nd < 400) snprintf(days[(*nd)++], 8, "%.6s", day); n->io.remove(n->io.user, tagnew); }
    csv_append(n, "/EVENTS.NEU", row);
    csv_append(n, tagnew, row);
    res->rows++;
    char wavp[48], svgp[48]; snprintf(wavp, sizeof wavp, "%.40s", fld[12 + o]); snprintf(svgp, sizeof svgp, "%.40s", fld[12 + o]);
    size_t l = strlen(svgp); memcpy(svgp + l - 3, "SVG", 3);
    n->ev_night = !strcmp(fld[2], "NACHT"); n->ev_limit = lim; n->ev_dist = (uint8_t)dist;
    n->ev_bg = bg; n->ev_lpeak = lpk; n->ev_lafmax = lafmax;
    if (rerender_event(n, wavp, svgp, laeq, lae, atoi(fld[0]), atoi(fld[0] + 5), atoi(fld[0] + 8), atoi(fld[1]), atoi(fld[1] + 3), atoi(fld[1] + 6)) == 0)
        res->svgs++;
}

int nc_recalibrate(nc_t *n, float delta_db, nc_recal_t *res)
{
    static char days[400][8];
    memset(res, 0, sizeof *res);
    void *f = n->io.open(n->io.user, "/EVENTS.CSV", 'r');
    if (!f) return -1;
    void *bk = n->io.open(n->io.user, "/EVENTS_ALT.CSV", 'w');           /* Originalkopie */
    n->io.remove(n->io.user, "/EVENTS.NEU");
    char rbuf[1024], line[512]; uint32_t ll = 0, nd = 0; int32_t got;
    while ((got = n->io.read(n->io.user, f, rbuf, sizeof rbuf)) > 0) {
        if (bk) n->io.write(n->io.user, bk, rbuf, (uint32_t)got);
        for (int32_t i = 0; i < got; i++) {
            if (rbuf[i] == '\n' || rbuf[i] == '\r') { if (ll) { line[ll] = 0; recal_line(n, line, delta_db, res, days, &nd); ll = 0; } }
            else if (ll < sizeof line - 1) line[ll++] = rbuf[i];
        }
    }
    if (ll) { line[ll] = 0; recal_line(n, line, delta_db, res, days, &nd); }
    n->io.close(n->io.user, f);
    if (bk) n->io.close(n->io.user, bk);
    if (res->rows == 0) { n->io.remove(n->io.user, "/EVENTS.NEU"); return -2; }
    if (copy_file(n, "/EVENTS.NEU", "/EVENTS.CSV") == 0) n->io.remove(n->io.user, "/EVENTS.NEU");
    for (uint32_t i = 0; i < nd; i++) {                                    /* Tagesdateien ersetzen, Tagesdiagramm neu */
        char src[32], dst[32]; snprintf(src, sizeof src, "/%.6s/TAG.NEU", days[i]); snprintf(dst, sizeof dst, "/%.6s/TAG.CSV", days[i]);
        if (copy_file(n, src, dst) == 0) n->io.remove(n->io.user, src);
        snprintf(n->dir, sizeof n->dir, "%.6s", days[i]);
        write_day_svg(n);
        res->days++;
    }
    int y, mo, d, h, mi, s, ms; nc_civil_from_ms(n->io.now_ms(n->io.user), &y, &mo, &d, &h, &mi, &s, &ms);
    void *lg = n->io.open(n->io.user, "/KORREKTUR.LOG", 'a');
    if (lg) {
        char dl[16]; fmt1(dl, sizeof dl, delta_db);
        io_printf(n, lg, "%04d-%02d-%02d %02d:%02d:%02d  Pegelkorrektur %s%s dB auf %lu Ereignisse angewendet (Original: EVENTS_ALT.CSV)\n",
                  y, mo, d, h, mi, s, delta_db >= 0 ? "+" : "", dl, (unsigned long)res->rows);
        n->io.close(n->io.user, lg);
    }
    return 0;
}
