#include "noise_core.h"
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
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

static void fmt1(char *out, size_t sz, float v)         /* "-12.3" ohne Float-printf */
{
    int t = (int)lroundf(v * 10.0f);
    int a = t < 0 ? -t : t;
    snprintf(out, sz, "%s%d.%d", t < 0 ? "-" : "", a / 10, a % 10);
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
    c->end_delta_db = 6.0f;
    c->end_hyst_db = 6.0f;
    c->pre_ms = 1000;
    c->post_ms = 3000;
    c->max_event_ms = 60000;
    c->slam_lafmax_dba = 75.0f;
    c->impulse_rise_db = 10.0f;
}

void nc_init(nc_t *n, const nc_config_t *c, const nc_io_t *io, int16_t *ring, uint32_t ring_len)
{
    memset(n, 0, sizeof *n);
    n->cfg = *c; n->io = *io; n->ring = ring; n->ring_len = ring_len;
    n->frame_len = c->fs / 100;
    double ref_pk = NC_FULLSCALE_24BIT * pow(10.0, NC_MIC_SENS_DBFS / 20.0);
    n->ref_peak = (float)ref_pk;
    n->ref_rms2 = (float)(ref_pk * ref_pk / 2.0);
    aw_init(n->aw, c->fs);
    n->ev_max_frames = c->max_event_ms / 10;
    n->ev_post_frames = c->post_ms / 10;
    if (n->ev_max_frames > NC_MAX_PLOT_BINS * 10 - 20) n->ev_max_frames = NC_MAX_PLOT_BINS * 10 - 20;
}

/* ----------------------------------------------------------- Ereignis-I/O */

static void drain(nc_t *n, int all)
{
    if (!n->wav) return;
    if (n->wr - n->rd > n->ring_len) {                      /* Ring ueberlaufen: SD zu langsam */
        uint64_t lost = n->wr - n->rd - n->ring_len;
        n->rd += lost; n->overruns += (uint32_t)lost;
    }
    while (n->wr - n->rd >= (all ? 1u : NC_CHUNK_SAMPLES)) {
        uint32_t avail = (uint32_t)(n->wr - n->rd);
        uint32_t cnt = avail < NC_CHUNK_SAMPLES ? avail : NC_CHUNK_SAMPLES;
        uint32_t ri = (uint32_t)(n->rd % n->ring_len);
        for (uint32_t i = 0; i < cnt; i++) {
            n->chunk[i] = n->ring[ri];
            if (++ri == n->ring_len) ri = 0;
        }
        n->io.write(n->io.user, n->wav, n->chunk, cnt * 2);
        n->wav_data_bytes += cnt * 2;
        n->rd += cnt;
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
    n->io.write(n->io.user, n->wav, b, WAV_DATA_START);
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

static const char *classify(const nc_t *n)
{
    if (n->ev_lafmax >= n->cfg.slam_lafmax_dba) return "KNALL";
    if (n->ev_rise >= n->cfg.impulse_rise_db) return "TUER";
    return "SONST";
}

static void write_svg(nc_t *n, const char *path, const char *cls, float laeq, float lae,
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
    int ymin = (int)floorf(vmin / 10.0f) * 10, ymax = (int)ceilf((vmax + 1.0f) / 10.0f) * 10;
    if (ymin < 0) ymin = 0;
    if (ymax - ymin < 30) ymax = ymin + 30;
    const int X0 = 70, Y0 = 70, PW = 800, PH = 320;
    float T = n->plot_n * 0.1f;
    if (T < 1.0f) T = 1.0f;
    char a[16], b2[16], c[16], e[16], g[16];
    fmt1(a, sizeof a, laeq); fmt1(b2, sizeof b2, n->ev_lafmax); fmt1(c, sizeof c, n->ev_lpeak);
    fmt1(e, sizeof e, lae); fmt1(g, sizeof g, n->ev_bg);

    io_printf(n, f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" "
              "width=\"900\" height=\"450\" viewBox=\"0 0 900 450\" font-family=\"sans-serif\" font-size=\"12\">\n"
              "<rect width=\"900\" height=\"450\" fill=\"#fff\"/>\n");
    io_printf(n, f, "<text x=\"%d\" y=\"26\" font-size=\"18\" font-weight=\"bold\">Lärmereignis %s – %04d-%02d-%02d %02d:%02d:%02d</text>\n",
              X0, cls, y, mo, d, h, mi, s);
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

static void finish_event(nc_t *n)
{
    drain(n, 1);
    /* Zeitmarken (cue + labl) hinter die Audiodaten */
    uint8_t *b = (uint8_t *)n->chunk, *p = b;
    uint32_t off[3] = { n->ev_trig_frame * n->frame_len, n->ev_lafmax_frame * n->frame_len,
                        (n->ev_last_active + 1) * n->frame_len };
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
    n->io.write(n->io.user, n->wav, b, (uint32_t)(p - b));

    /* Kennwerte */
    uint32_t cn = n->ev_n_snap ? n->ev_n_snap : 1;
    float laeq = 10.0f * log10f((float)(n->ev_sumA_snap / cn) / n->ref_rms2) + NC_REF_SPL_DB + n->cfg.cal_offset_db;
    float lae = laeq + 10.0f * log10f(cn * 0.01f);
    const char *cls = classify(n);
    char a[16], bb[16], c[16], desc[256];
    fmt1(a, sizeof a, laeq); fmt1(bb, sizeof bb, n->ev_lafmax); fmt1(c, sizeof c, n->ev_lpeak);
    snprintf(desc, sizeof desc, "%s LAeq=%s dB(A) LAFmax=%s dB(A) Lpeak=%s dB", cls, a, bb, c);
    uint32_t total = WAV_DATA_START + n->wav_data_bytes + (uint32_t)(p - b);
    uint8_t hdr[4];
    n->io.seek(n->io.user, n->wav, 4);   put32(hdr, total - 8);              n->io.write(n->io.user, n->wav, hdr, 4);
    n->io.seek(n->io.user, n->wav, 650); put32(hdr, n->wav_data_bytes);       n->io.write(n->io.user, n->wav, hdr, 4);
    uint8_t dz[256]; memset(dz, 0, sizeof dz); memcpy(dz, desc, strlen(desc));
    n->io.seek(n->io.user, n->wav, WAV_BEXT_DESC); n->io.write(n->io.user, n->wav, dz, sizeof dz);
    n->io.close(n->io.user, n->wav);
    n->wav = NULL;

    if (n->bin_cnt) { if (n->plot_n < NC_MAX_PLOT_BINS) n->plot[n->plot_n++] = n->bin_max; n->bin_cnt = 0; }

    int y, mo, d, h, mi, s, ms;
    nc_civil_from_ms(n->ev_start_ms, &y, &mo, &d, &h, &mi, &s, &ms);
    char path[32];
    snprintf(path, sizeof path, "/%s/%s.SVG", n->dir, n->base);
    write_svg(n, path, cls, laeq, lae, y, mo, d, h, mi, s);

    void *f = n->io.open(n->io.user, "/EVENTS.CSV", 'r');   /* Existenz pruefen */
    int fresh = (f == NULL);
    if (f) n->io.close(n->io.user, f);
    f = n->io.open(n->io.user, "/EVENTS.CSV", 'a');
    if (f) {
        char dur[16], lp[16], bgs[16];
        uint32_t cs = n->ev_last_active + 1 - n->ev_trig_frame;   /* aktive Dauer in 10-ms-Frames */
        snprintf(dur, sizeof dur, "%lu.%02lu", (unsigned long)(cs / 100), (unsigned long)(cs % 100));
        fmt1(lp, sizeof lp, lae);
        fmt1(bgs, sizeof bgs, n->ev_bg);
        if (fresh)
            io_printf(n, f, "datum;zeit;typ;aktiv_s;LAeq_dBA;LAFmax_dBA;Lpeak_dB;LAE_dBA;hintergrund_dBA;datei;ueberlaeufe\n");
        io_printf(n, f, "%04d-%02d-%02d;%02d:%02d:%02d.%03d;%s;%s;%s;%s;%s;%s;%s;/%s/%s.WAV;%lu\n",
                  y, mo, d, h, mi, s, ms, cls, dur, a, bb, c, lp, bgs, n->dir, n->base, (unsigned long)n->overruns);
        n->io.close(n->io.user, f);
    }
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

static void event_account(nc_t *n, float L, float laf, float lpk, double meansq, uint32_t idx)
{
    n->ev_sumA += meansq; n->ev_n++;
    if (L >= end_threshold(n)) {
        n->ev_last_active = idx;
        n->ev_sumA_snap = n->ev_sumA; n->ev_n_snap = n->ev_n;
    }
    if (laf > n->ev_lafmax) { n->ev_lafmax = laf; n->ev_lafmax_frame = idx; }
    if (lpk > n->ev_lpeak) n->ev_lpeak = lpk;
    if (L - n->prev_db > n->ev_rise) n->ev_rise = L - n->prev_db;
}

static void start_event(nc_t *n, float L, float laf, float lpk, double meansq)
{
    uint32_t ring_frames = n->ring_len / n->frame_len;
    uint32_t pre = n->cfg.pre_ms / 10;
    if (pre > n->frames_seen - 1) pre = n->frames_seen - 1;
    if (pre > n->hist_n - 1) pre = n->hist_n - 1;
    uint32_t pre_max = ring_frames > 50 ? ring_frames - 50 : ring_frames / 2;   /* 0.5 s Reserve fuer SD-Wartezeiten */
    if (pre > pre_max) pre = pre_max;
    pre = pre / 10 * 10;
    n->ev_pre_frames = pre;
    n->rd = n->wr - (uint64_t)n->frame_len * (pre + 1);
    int64_t now = n->io.now_ms(n->io.user);
    n->ev_start_ms = now - (int64_t)n->frame_len * (pre + 1) * 1000 / n->cfg.fs;
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

    n->plot_n = 0; n->bin_cnt = 0;
    for (uint32_t i = 0; i <= pre; i++)                    /* Vorlauf aus der Historie + Ausloeseframe */
        plot_add(n, (int16_t)n->hist[(n->hist_n - 1 - pre + i) % NC_HIST_FRAMES]);
    n->state = NC_ACTIVE;
    n->ev_frames = pre + 1;
    n->ev_trig_frame = pre;
    n->ev_last_active = pre;
    n->ev_sumA = 0; n->ev_n = 0; n->ev_sumA_snap = 0; n->ev_n_snap = 0;
    n->ev_lafmax = -999; n->ev_lpeak = -999; n->ev_rise = -999;
    n->ev_bg = n->bg_db;
    event_account(n, L, laf, lpk, meansq, pre);
}

static void end_frame(nc_t *n)
{
    double meansq = n->frame_sumA / n->frame_len;
    if (meansq < 1e-3) meansq = 1e-3;
    float cal = NC_REF_SPL_DB + n->cfg.cal_offset_db;
    float L = 10.0f * log10f((float)meansq / n->ref_rms2) + cal;
    float pk = n->frame_peak < 1 ? 1.0f : (float)n->frame_peak;
    float lpk = 20.0f * log10f(pk / n->ref_peak) + cal;
    n->frame_sumA = 0; n->frame_peak = 0; n->frame_pos = 0;

    const float alpha = 1.0f - expf(-0.010f / 0.125f);     /* Fast: tau = 125 ms */
    if (n->frames_seen == 0) n->laf_energy = (float)meansq;
    else n->laf_energy += alpha * ((float)meansq - n->laf_energy);
    float laf = 10.0f * log10f(n->laf_energy / n->ref_rms2) + cal;
    float lafc = laf < 0 ? 0 : laf;
    n->hist[n->hist_n % NC_HIST_FRAMES] = (uint16_t)(lafc * 10.0f + 0.5f);
    n->hist_n++;
    n->frames_seen++;

    if (n->state == NC_IDLE) {
        float thr = n->bg_db + n->cfg.trig_delta_db;
        if (thr < n->cfg.trig_min_dba) thr = n->cfg.trig_min_dba;
        if (n->frames_seen <= 100) {
            n->bg_db = n->frames_seen == 1 ? L : n->bg_db + (L - n->bg_db) * 0.05f;
        } else if (n->holdoff) {
            n->holdoff--;
        } else if (L >= thr) {
            start_event(n, L, laf, lpk, meansq);
        } else if (L < n->bg_db) {
            n->bg_db += (L - n->bg_db) * 0.05f;
        } else if (L < n->bg_db + 6.0f) {
            n->bg_db += (L - n->bg_db) * 0.0015f;
        }
    } else {
        uint32_t idx = n->ev_frames++;
        plot_add(n, (int16_t)n->hist[(n->hist_n - 1) % NC_HIST_FRAMES]);
        event_account(n, L, laf, lpk, meansq, idx);
        drain(n, 0);
        if (idx - n->ev_last_active >= n->ev_post_frames || n->ev_frames >= n->ev_max_frames)
            finish_event(n);
    }
    n->prev_db = L;
}

void nc_process(nc_t *n, const int32_t *s, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        int32_t x = s[i];
        n->ring[n->widx] = (int16_t)(x >> 8);
        if (++n->widx == n->ring_len) n->widx = 0;
        n->wr++;
        float y = aw_run(n->aw, (float)x);
        n->frame_sumA += (double)(y * y);
        int32_t ax = x < 0 ? -x : x;
        if (ax > n->frame_peak) n->frame_peak = ax;
        if (++n->frame_pos == n->frame_len) end_frame(n);
    }
}

void nc_flush(nc_t *n)
{
    if (n->state == NC_ACTIVE && n->wav) finish_event(n);
}
