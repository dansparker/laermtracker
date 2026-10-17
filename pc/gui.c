/*
 * Laermtracker - Windows-Oberflaeche (reines C, Win32-API)
 * Gleicher Kern wie Firmware und Kommandozeilenversion (core/noise_core.c); Eingang: USB-/Systemmikrofon (WinMM).
 *
 *   laermtracker-gui.exe
 *   laermtracker-gui.exe --selftest datei.wav --start "2026-10-03 21:59:30" --out ordner   (Test: WAV statt Mikrofon, beendet sich selbst)
 */
#define UNICODE
#define _UNICODE
#define _WIN32_IE 0x0600
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include "../core/noise_core.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { IDC_DEV = 100, IDC_SENS, IDC_CAL, IDC_OUT, IDC_BROWSE, IDC_OPEN, IDC_KORR, IDC_RECAL, IDC_TRIGMIN, IDC_DELTA, IDC_POST, IDC_LIMN, IDC_LIMD,
       IDC_ONLYDOOR, IDC_START, IDC_STATUS, IDC_LIST, IDC_GRAPH, IDC_DAYPLOT, IDC_CSV, IDC_TIMER = 1 };
#define WM_APP_EVENT (WM_APP + 1)     /* lParam: malloc'ierte letzte CSV-Zeile (UTF-16) */
#define WM_APP_DONE  (WM_APP + 2)     /* Arbeits-Thread beendet */
#define WM_APP_CAL   (WM_APP + 3)     /* Kalibrierung fertig (g_cal_result) */
#define WM_APP_SELFTEST (WM_APP + 10)

#define NBUF 20
#define HIST_N 600                    /* 120 s bei 5 Werten/s */

static HWND g_korr, g_hwnd, g_dev, g_sens, g_out_e, g_trigmin, g_delta, g_post, g_limn, g_limd, g_onlydoor, g_start, g_status, g_list, g_graph, g_cal;
static HFONT g_font;
static nc_t g_nc;
static char g_out[520];
static uint32_t g_fs = 48000;
static float g_cal_db;
static volatile LONG g_stop, g_running;
static HANDLE g_thread;
static int g_mode;                   /* 0 Ueberwachung, 1 Kalibrierung, 2 WAV-Datei (Selbsttest) */
static int g_dev_index;
static double g_cal_result;
static uint64_t g_pushed;
static int g_sim; static int64_t g_sim_start_ms;
static wchar_t g_wavin[520];
static float g_hist[HIST_N]; static uint8_t g_histrec[HIST_N]; static int g_hist_n, g_hist_pos;
static int g_selftest;
static uint32_t g_seen_total, g_seen_dropped;

/* ------------------------------------------------------------ Zeichenkonvertierung / Dateien (UTF-8 fuer den Kern) */
static void u2w(const char *s, wchar_t *o, int n) { MultiByteToWideChar(CP_UTF8, 0, s, -1, o, n); }
static void w2u(const wchar_t *s, char *o, int n) { WideCharToMultiByte(CP_UTF8, 0, s, -1, o, n, NULL, NULL); }

static int64_t io_now(void *u)
{
    (void)u;
    if (g_sim) return g_sim_start_ms + (int64_t)(g_pushed * 1000ull / g_fs);
    SYSTEMTIME st; GetLocalTime(&st);
    return nc_ms_from_civil(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}
static void fullw(wchar_t *o, int n, const char *p) { char b[700]; snprintf(b, sizeof b, "%s%s", g_out, p); u2w(b, o, n); }
static int io_mkdir(void *u, const char *p) { (void)u; wchar_t w[700]; fullw(w, 700, p); _wmkdir(w); return 0; }
static void *io_open(void *u, const char *p, char m)
{
    (void)u; wchar_t w[700]; fullw(w, 700, p);
    return _wfopen(w, m == 'r' ? L"rb" : (m == 'a' ? L"ab" : L"wb"));
}
static int io_write(void *u, void *h, const void *b, uint32_t n) { (void)u; return fwrite(b, 1, n, (FILE *)h) == n ? 0 : -1; }
static int io_seek(void *u, void *h, uint32_t p) { (void)u; return fseek((FILE *)h, (long)p, SEEK_SET); }
static int io_close(void *u, void *h) { (void)u; return fclose((FILE *)h); }
static int32_t io_read(void *u, void *h, void *b, uint32_t n) { (void)u; return (int32_t)fread(b, 1, n, (FILE *)h); }
static int io_remove(void *u, const char *p) { (void)u; wchar_t w[700]; fullw(w, 700, p); return _wremove(w); }

/* ------------------------------------------------------------ Einstellungen (INI in %APPDATA%\Laermtracker) */
static void ini_path(wchar_t *p)
{
    wchar_t dir[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir) != S_OK) wcscpy(dir, L".");
    _snwprintf(p, MAX_PATH, L"%ls\\Laermtracker", dir);
    CreateDirectoryW(p, NULL);
    wcscat(p, L"\\settings.ini");
}
static void ini_get(const wchar_t *key, const wchar_t *def, wchar_t *out, int n)
{
    wchar_t p[MAX_PATH]; ini_path(p); GetPrivateProfileStringW(L"laermtracker", key, def, out, n, p);
}
static void ini_set(const wchar_t *key, const wchar_t *val) { wchar_t p[MAX_PATH]; ini_path(p); WritePrivateProfileStringW(L"laermtracker", key, val, p); }
static void save_settings(void)
{
    wchar_t t[600]; const struct { const wchar_t *k; HWND h; } f[] = { { L"sens", g_sens }, { L"korr", g_korr }, { L"out", g_out_e }, { L"trigmin", g_trigmin }, { L"delta", g_delta },
        { L"post", g_post }, { L"limn", g_limn }, { L"limd", g_limd } };
    for (unsigned i = 0; i < sizeof f / sizeof *f; i++) { GetWindowTextW(f[i].h, t, 600); ini_set(f[i].k, t); }
    _snwprintf(t, 20, L"%d", (int)SendMessageW(g_dev, CB_GETCURSEL, 0, 0)); ini_set(L"device", t);
    ini_set(L"onlydoor", SendMessageW(g_onlydoor, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
}

/* ------------------------------------------------------------ WAV lesen (nur Selbsttest) */
static int16_t *read_wav(const wchar_t *path, uint32_t *rate, uint32_t *count)
{
    FILE *f = _wfopen(path, L"rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len); if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return NULL; }
    fclose(f);
    int16_t *out = NULL; uint32_t ch = 1; long pos = 12;
    if (len < 44 || memcmp(d, "RIFF", 4)) { free(d); return NULL; }
    while (pos + 8 <= len) {
        uint32_t sz = d[pos + 4] | d[pos + 5] << 8 | d[pos + 6] << 16 | (uint32_t)d[pos + 7] << 24; const uint8_t *c = d + pos + 8;
        if (!memcmp(d + pos, "fmt ", 4)) { ch = c[2] | c[3] << 8; *rate = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24; }
        else if (!memcmp(d + pos, "data", 4)) {
            if ((long)sz > len - pos - 8) sz = (uint32_t)(len - pos - 8);
            uint32_t n = sz / 2 / ch; out = malloc((size_t)n * 2);
            for (uint32_t i = 0; i < n; i++) out[i] = (int16_t)(c[i * ch * 2] | c[i * ch * 2 + 1] << 8);
            *count = n; break;
        }
        pos += 8 + (long)sz + (sz & 1);
    }
    free(d); return out;
}

/* ------------------------------------------------------------ Kern fuettern */
static void feed(const int16_t *s, uint32_t n)
{
    static int32_t blk[4096];
    while (n) {
        uint32_t m = n < 4096 ? n : 4096;
        for (uint32_t i = 0; i < m; i++) blk[i] = (int32_t)s[i] * 256;
        nc_push24(&g_nc, blk, m); g_pushed += m;
        nc_poll(&g_nc);
        s += m; n -= m;
    }
}

static float level_laf(void)
{
    float e = g_nc.laf_energy; if (e < 1e-6f) e = 1e-6f;
    return 10.0f * log10f(e / g_nc.ref_rms2) + NC_REF_SPL_DB + g_cal_db;
}

/* letzte Zeile von EVENTS.CSV als UTF-16 an die Oberflaeche melden */
static void post_last_event(void)
{
    wchar_t w[700]; fullw(w, 700, "/EVENTS.CSV");
    FILE *f = _wfopen(w, L"rb"); if (!f) return;
    char buf[1025]; fseek(f, 0, SEEK_END); long len = ftell(f); long from = len > 1024 ? len - 1024 : 0;
    fseek(f, from, SEEK_SET); size_t n = fread(buf, 1, 1024, f); fclose(f); buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    char *line = strrchr(buf, '\n'); line = line ? line + 1 : buf;
    wchar_t *ws = malloc(1024 * sizeof(wchar_t)); u2w(line, ws, 1024);
    PostMessageW(g_hwnd, WM_APP_EVENT, 0, (LPARAM)ws);
}

static void check_events(void)
{
    if (g_nc.events_total != g_seen_total) { g_seen_total = g_nc.events_total; post_last_event(); }
    g_seen_dropped = g_nc.events_dropped;
}

/* ------------------------------------------------------------ Arbeits-Thread */
static DWORD WINAPI worker(LPVOID p)
{
    (void)p;
    if (g_mode == 2) {                                   /* WAV statt Mikrofon (Selbsttest) */
        uint32_t r = 0, n = 0; int16_t *w = read_wav(g_wavin, &r, &n);
        if (w) {
            for (uint32_t i = 0; i < n && !g_stop; i += g_fs / 10) { uint32_t m = n - i < g_fs / 10 ? n - i : g_fs / 10; feed(w + i, m); check_events(); }
            nc_flush(&g_nc); check_events(); free(w);
        }
        InterlockedExchange(&g_running, 0); PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return 0;
    }
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, g_fs, g_fs * 2, 2, 16, 0 };
    HANDLE ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    HWAVEIN hwi;
    if (waveInOpen(&hwi, g_dev_index < 0 ? WAVE_MAPPER : (UINT)g_dev_index, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        MessageBoxW(g_hwnd, L"Das Mikrofon kann nicht geöffnet werden (belegt oder keine Berechtigung).\n"
                    L"Windows: Einstellungen → Datenschutz → Mikrofon → Zugriff für Desktop-Apps erlauben.", L"Laermtracker", MB_ICONERROR);
        InterlockedExchange(&g_running, 0); PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return 1;
    }
    uint32_t blen = g_fs / 10; WAVEHDR hdr[NBUF]; int16_t *buf[NBUF];
    for (int i = 0; i < NBUF; i++) {
        buf[i] = malloc(blen * 2); memset(&hdr[i], 0, sizeof hdr[i]); hdr[i].lpData = (LPSTR)buf[i]; hdr[i].dwBufferLength = blen * 2;
        waveInPrepareHeader(hwi, &hdr[i], sizeof hdr[i]); waveInAddBuffer(hwi, &hdr[i], sizeof hdr[i]);
    }
    waveInStart(hwi);
    int next = 0; uint64_t total = 0, ncal = 0; double sum = 0, sumsq = 0;
    while (!g_stop) {
        WaitForSingleObject(ev, 200);
        while (hdr[next].dwFlags & WHDR_DONE) {
            uint32_t n = hdr[next].dwBytesRecorded / 2;
            if (g_mode == 1) {
                if (total >= g_fs) for (uint32_t i = 0; i < n; i++) { sum += buf[next][i]; sumsq += (double)buf[next][i] * buf[next][i]; ncal++; }
                total += n;
            } else { feed(buf[next], n); check_events(); }
            waveInAddBuffer(hwi, &hdr[next], sizeof hdr[next]); next = (next + 1) % NBUF;
        }
        if (g_mode == 1 && total >= (uint64_t)g_fs * 6) break;
    }
    waveInStop(hwi); waveInReset(hwi);
    for (int i = 0; i < NBUF; i++) { waveInUnprepareHeader(hwi, &hdr[i], sizeof hdr[i]); free(buf[i]); }
    waveInClose(hwi); CloseHandle(ev);
    if (g_mode == 1) {
        if (ncal) { double m = sum / ncal, rms = sqrt(sumsq / ncal - m * m); g_cal_result = 20.0 * log10(rms * 1.41421356 / 32768.0); }
        else g_cal_result = -1000;
        InterlockedExchange(&g_running, 0); PostMessageW(g_hwnd, WM_APP_CAL, 0, 0); PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return 0;
    }
    nc_flush(&g_nc); check_events();
    InterlockedExchange(&g_running, 0); PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

/* ------------------------------------------------------------ Oberflaeche */
static double edit_d(HWND h, double def)
{
    wchar_t t[64]; GetWindowTextW(h, t, 64);
    for (wchar_t *c = t; *c; c++) if (*c == L',') *c = L'.';
    wchar_t *e; double v = wcstod(t, &e); return e == t ? def : v;
}
static void set_edit_d(HWND h, double v, int dec) { wchar_t t[64]; _snwprintf(t, 64, L"%.*f", dec, v); SetWindowTextW(h, t); }

static void set_controls_enabled(int running)
{
    HWND h[] = { GetDlgItem(g_hwnd, IDC_RECAL), g_korr, g_dev, g_sens, g_out_e, g_trigmin, g_delta, g_post, g_limn, g_limd, g_onlydoor, g_cal, GetDlgItem(g_hwnd, IDC_BROWSE) };
    for (unsigned i = 0; i < sizeof h / sizeof *h; i++) EnableWindow(h[i], !running);
    SetWindowTextW(g_start, g_running && g_mode == 0 ? L"■ Stopp" : L"▶ Start");
    EnableWindow(g_start, !(running && g_mode != 0));
}

static int start_job(int mode)
{
    if (g_running) return 0;
    g_mode = mode;
    wchar_t t[600]; GetWindowTextW(g_out_e, t, 600); w2u(t, g_out, sizeof g_out);
    if (!g_out[0]) { MessageBoxW(g_hwnd, L"Bitte einen Ausgabeordner angeben.", L"Laermtracker", MB_ICONINFORMATION); return 0; }
    wchar_t w[600]; u2w(g_out, w, 600); SHCreateDirectoryExW(NULL, w, NULL);
    int sel = (int)SendMessageW(g_dev, CB_GETCURSEL, 0, 0); g_dev_index = sel - 1;     /* 0 = Standardgerät */
    double sens = edit_d(g_sens, NC_MIC_SENS_DBFS);
    g_cal_db = (float)(-sens - 26.0 + edit_d(g_korr, 0));        /* Korrektur wirkt wie eine feinere Kalibrierung */
    nc_config_t cfg; nc_default_config(&cfg);
    cfg.fs = g_fs; cfg.cal_offset_db = g_cal_db;
    cfg.trig_min_dba = (float)edit_d(g_trigmin, 50); cfg.trig_delta_db = (float)edit_d(g_delta, 12);
    cfg.post_ms = (uint32_t)(edit_d(g_post, 3) * 1000.0);
    cfg.lim_night_dba = (float)edit_d(g_limn, 42); cfg.lim_day_dba = (float)edit_d(g_limd, 55);
    cfg.keep_sonst = SendMessageW(g_onlydoor, BM_GETCHECK, 0, 0) == BST_CHECKED ? 0 : 1;
    if (mode != 1) {
        static int16_t *ring; free(ring);
        uint32_t ring_len = g_fs * 4; ring = malloc((size_t)ring_len * 2);
        nc_io_t io = { NULL, io_now, io_mkdir, io_open, io_write, io_seek, io_close, io_read, io_remove };
        nc_init(&g_nc, &cfg, &io, ring, ring_len);
        g_pushed = 0; g_seen_total = g_seen_dropped = 0; g_hist_n = g_hist_pos = 0;
    }
    g_stop = 0; InterlockedExchange(&g_running, 1);
    set_controls_enabled(1);
    g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (mode == 0) save_settings();
    return 1;
}

static void stop_job(void)
{
    if (!g_running) return;
    g_stop = 1;
    if (g_thread) { WaitForSingleObject(g_thread, 15000); CloseHandle(g_thread); g_thread = NULL; }
}

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id, DWORD ex)
{
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return c;
}
static HWND label(const wchar_t *t, int x, int y, int w) { return mk(L"STATIC", t, SS_LEFT, x, y + 3, w, 18, -1, 0); }
static HWND edit(int id, const wchar_t *t, int x, int y, int w) { return mk(L"EDIT", t, ES_AUTOHSCROLL, x, y, w, 22, id, WS_EX_CLIENTEDGE); }

static void list_col(int i, const wchar_t *t, int w)
{
    LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH, 0, w, (LPWSTR)t, 0, 0, 0, 0 };
    SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
}

static void add_event_row(wchar_t *line)
{
    wchar_t *f[16]; int nf = 0; f[nf++] = line;
    for (wchar_t *p = line; *p && nf < 16; p++) if (*p == L';') { *p = 0; f[nf++] = p + 1; }
    if (nf < 13) return;
    wchar_t zeit[48]; _snwprintf(zeit, 48, L"%ls %.8ls", f[0], f[1]);
    const wchar_t *typ = !wcscmp(f[3], L"TUER") ? L"Tür" : (!wcscmp(f[3], L"KNALL") ? L"Knall" : L"Sonstiges");
    wchar_t col[7][48];
    wcscpy(col[0], zeit); wcscpy(col[1], typ); _snwprintf(col[2], 48, L"%ls dB(A)", f[6]); _snwprintf(col[3], 48, L"%ls dB(A)", f[5]);
    _snwprintf(col[4], 48, L"%ls s", f[4]); wcscpy(col[5], !wcscmp(f[2], L"NACHT") ? L"Nacht" : L"Tag");
    wcscpy(col[6], f[12][0] == L'1' ? L"STÖRUNG" : L"–");
    int bad = f[12][0] == L'1';
    LVITEMW it = { LVIF_TEXT | LVIF_PARAM, 0, 0, 0, 0, col[0], 0, 0, bad };
    int idx = (int)SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    for (int c = 1; c < 7; c++) { LVITEMW s = { LVIF_TEXT, idx, c, 0, 0, col[c], 0, 0, 0 }; SendMessageW(g_list, LVM_SETITEMTEXTW, idx, (LPARAM)&s); }
}

static void open_path(const wchar_t *rel, int explore)
{
    wchar_t base[700], p[800]; u2w(g_out, base, 700);
    if (!g_out[0]) { GetWindowTextW(g_out_e, base, 700); }
    _snwprintf(p, 800, L"%ls%ls", base, rel);
    ShellExecuteW(NULL, explore ? L"explore" : L"open", p, NULL, NULL, SW_SHOWNORMAL);
}

/* Pegelverlauf zeichnen */
static LRESULT CALLBACK graph_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, r.right, r.bottom); SelectObject(mem, bm);
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255)); FillRect(mem, &r, bg); DeleteObject(bg);
        SelectObject(mem, g_font); SetBkMode(mem, TRANSPARENT);
        const int L0 = 44, T0 = 8, B0 = 6, R0 = 8; int pw = r.right - L0 - R0, ph = r.bottom - T0 - B0; const float lo = 20, hi = 100;
        HPEN grid = CreatePen(PS_SOLID, 1, RGB(225, 225, 225));
        SelectObject(mem, grid);
        for (int v = 20; v <= 100; v += 10) {
            int y = T0 + (int)(ph * (hi - v) / (hi - lo)); MoveToEx(mem, L0, y, NULL); LineTo(mem, L0 + pw, y);
            wchar_t t[8]; _snwprintf(t, 8, L"%d", v); TextOutW(mem, 8, y - 7, t, (int)wcslen(t));
        }
        DeleteObject(grid);
        if (g_hist_n > 1) {
            float bg = g_nc.bg_db, thr = bg + g_nc.cfg.trig_delta_db; if (thr < g_nc.cfg.trig_min_dba) thr = g_nc.cfg.trig_min_dba;
            HPEN pthr = CreatePen(PS_DOT, 1, RGB(224, 138, 0)); SelectObject(mem, pthr);
            int yt = T0 + (int)(ph * (hi - thr) / (hi - lo)); MoveToEx(mem, L0, yt, NULL); LineTo(mem, L0 + pw, yt); DeleteObject(pthr);
            HPEN pn = CreatePen(PS_SOLID, 2, RGB(23, 105, 170)), pr = CreatePen(PS_SOLID, 2, RGB(211, 47, 47));
            int n = g_hist_n; int start = (g_hist_pos - n + HIST_N) % HIST_N;
            for (int i = 0; i < n; i++) {
                float v = g_hist[(start + i) % HIST_N]; if (v < lo) v = lo; if (v > hi) v = hi;
                int x = L0 + (int)((long long)pw * (HIST_N - n + i) / (HIST_N - 1)), y = T0 + (int)(ph * (hi - v) / (hi - lo));
                SelectObject(mem, g_histrec[(start + i) % HIST_N] ? pr : pn);
                if (i == 0) MoveToEx(mem, x, y, NULL); else LineTo(mem, x, y);
            }
            DeleteObject(pn); DeleteObject(pr);
        }
        { const wchar_t *tt = L"LAF dB(A) – letzte 2 Minuten (rot = Aufnahme, orange = Auslöseschwelle)"; TextOutW(mem, L0 + 4, T0 + 2, tt, (int)wcslen(tt)); }
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        DeleteObject(bm); DeleteDC(mem); EndPaint(h, &ps); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void layout(int W, int H)
{
    MoveWindow(g_graph, 10, 134, W - 20, 150, TRUE);
    MoveWindow(g_list, 10, 292, W - 20, H - 292 - 44, TRUE);
    MoveWindow(GetDlgItem(g_hwnd, IDC_DAYPLOT), 10, H - 36, 200, 28, TRUE);
    MoveWindow(GetDlgItem(g_hwnd, IDC_CSV), 220, H - 36, 200, 28, TRUE);
    MoveWindow(g_status, 475, 104, W - 485, 20, TRUE);
}

static void refresh_status(void)
{
    wchar_t t[400];
    if (g_running && g_mode == 1) { SetWindowTextW(g_status, L"Kalibrierung läuft … Kalibrator (94 dB, 1 kHz) am Mikrofon halten (6 s)"); return; }
    if (!g_running) { _snwprintf(t, 400, L"Angehalten.  Gespeichert: %lu, verworfen: %lu", (unsigned long)g_nc.events_total, (unsigned long)g_nc.events_dropped); SetWindowTextW(g_status, g_nc.ring ? t : L"Bereit. Mikrofon und Ordner wählen, dann Start."); return; }
    float laf = level_laf(), thr = g_nc.bg_db + g_nc.cfg.trig_delta_db; if (thr < g_nc.cfg.trig_min_dba) thr = g_nc.cfg.trig_min_dba;
    _snwprintf(t, 400, L"%ls  LAF %.1f dB(A) | Hintergrund %.1f | Auslösung ab %.1f | gespeichert %lu, verworfen %lu",
             g_nc.state == NC_ACTIVE ? L"● AUFNAHME" : L"Überwachung läuft", laf, g_nc.bg_db, thr, (unsigned long)g_nc.events_total, (unsigned long)g_nc.events_dropped);
    SetWindowTextW(g_status, t);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        NONCLIENTMETRICSW ncm = { sizeof ncm }; if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        label(L"Mikrofon:", 10, 10, 70);
        g_dev = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 85, 8, 270, 200, IDC_DEV, 0);
        label(L"Empfindlichkeit [dBFS @94 dB]:", 365, 10, 195); g_sens = edit(IDC_SENS, L"-26", 560, 8, 52);
        label(L"Korrektur [dB]:", 622, 10, 95); g_korr = edit(IDC_KORR, L"0", 718, 8, 50);
        g_cal = mk(L"BUTTON", L"Kalibrieren …", BS_PUSHBUTTON, 778, 7, 120, 24, IDC_CAL, 0);
        label(L"Ausgabeordner:", 10, 42, 90); g_out_e = edit(IDC_OUT, L"", 105, 40, 395);
        mk(L"BUTTON", L"…", BS_PUSHBUTTON, 505, 39, 30, 24, IDC_BROWSE, 0);
        mk(L"BUTTON", L"Ordner öffnen", BS_PUSHBUTTON, 540, 39, 110, 24, IDC_OPEN, 0);
        mk(L"BUTTON", L"Vorhandene Aufnahmen umrechnen …", BS_PUSHBUTTON, 660, 39, 238, 24, IDC_RECAL, 0);
        label(L"Auslösung ab [dB(A)]:", 10, 72, 130); g_trigmin = edit(IDC_TRIGMIN, L"50", 140, 70, 40);
        label(L"+ über Hintergrund [dB]:", 195, 72, 150); g_delta = edit(IDC_DELTA, L"12", 345, 70, 40);
        label(L"Nachlauf [s]:", 400, 72, 80); g_post = edit(IDC_POST, L"3", 480, 70, 40);
        label(L"Grenzwert Nacht / Tag [dB(A)]:", 535, 72, 170); g_limn = edit(IDC_LIMN, L"42", 710, 70, 40); g_limd = edit(IDC_LIMD, L"55", 755, 70, 40);
        g_start = mk(L"BUTTON", L"▶ Start", BS_DEFPUSHBUTTON, 10, 98, 140, 30, IDC_START, 0);
        g_onlydoor = mk(L"BUTTON", L"Nur Tür/Knall behalten (Stimmen verwerfen)", BS_AUTOCHECKBOX, 165, 104, 300, 20, IDC_ONLYDOOR, 0);
        g_status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 475, 104, 440, 20, IDC_STATUS, 0);
        g_graph = CreateWindowExW(WS_EX_CLIENTEDGE, L"LevelGraph", L"", WS_CHILD | WS_VISIBLE, 10, 134, 860, 150, h, (HMENU)IDC_GRAPH, GetModuleHandle(NULL), NULL);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 10, 292, 860, 250, h, (HMENU)IDC_LIST, GetModuleHandle(NULL), NULL);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        list_col(0, L"Zeit", 170); list_col(1, L"Art", 90); list_col(2, L"LAFmax", 100); list_col(3, L"LAeq", 100); list_col(4, L"Dauer", 80); list_col(5, L"Zeitraum", 80); list_col(6, L"Beurteilung", 110);
        mk(L"BUTTON", L"Tagesdiagramm öffnen", BS_PUSHBUTTON, 10, 0, 200, 28, IDC_DAYPLOT, 0);
        mk(L"BUTTON", L"Ereignisliste (CSV) öffnen", BS_PUSHBUTTON, 220, 0, 200, 28, IDC_CSV, 0);
        /* Geräte */
        SendMessageW(g_dev, CB_ADDSTRING, 0, (LPARAM)L"Standardmikrofon (Windows)");
        UINT n = waveInGetNumDevs();
        for (UINT i = 0; i < n; i++) { WAVEINCAPSW c; if (waveInGetDevCaps(i, &c, sizeof c) == MMSYSERR_NOERROR) SendMessageW(g_dev, CB_ADDSTRING, 0, (LPARAM)c.szPname); }
        /* gespeicherte Einstellungen */
        wchar_t t[600]; wchar_t def[MAX_PATH] = L"";
        if (SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, def) == S_OK) wcscat(def, L"\\Laermtracker");
        ini_get(L"device", L"0", t, 20); SendMessageW(g_dev, CB_SETCURSEL, _wtoi(t) < (int)(n + 1) ? _wtoi(t) : 0, 0);
        ini_get(L"sens", L"-26", t, 60); SetWindowTextW(g_sens, t);
        ini_get(L"korr", L"0", t, 60); SetWindowTextW(g_korr, t);
        ini_get(L"out", def, t, 600); SetWindowTextW(g_out_e, t);
        ini_get(L"trigmin", L"50", t, 60); SetWindowTextW(g_trigmin, t);
        ini_get(L"delta", L"12", t, 60); SetWindowTextW(g_delta, t);
        ini_get(L"post", L"3", t, 60); SetWindowTextW(g_post, t);
        ini_get(L"limn", L"42", t, 60); SetWindowTextW(g_limn, t);
        ini_get(L"limd", L"55", t, 60); SetWindowTextW(g_limd, t);
        ini_get(L"onlydoor", L"0", t, 8); SendMessageW(g_onlydoor, BM_SETCHECK, t[0] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
        SetTimer(h, IDC_TIMER, 200, NULL);
        refresh_status();
        return 0; }
    case WM_SIZE: layout(LOWORD(l), HIWORD(l)); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)l)->ptMinTrackSize.x = 920; ((MINMAXINFO *)l)->ptMinTrackSize.y = 520; return 0;
    case WM_TIMER:
        if (g_running && g_mode == 0) {
            g_hist[g_hist_pos] = level_laf(); g_histrec[g_hist_pos] = g_nc.state == NC_ACTIVE;
            g_hist_pos = (g_hist_pos + 1) % HIST_N; if (g_hist_n < HIST_N) g_hist_n++;
            InvalidateRect(g_graph, NULL, FALSE);
        }
        refresh_status();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_START: if (g_running) stop_job(); else start_job(0); return 0;
        case IDC_CAL:
            if (MessageBoxW(h, L"Kalibrator (94 dB, 1 kHz) dicht an das Mikrofon halten und mit OK die 6-Sekunden-Messung starten.\n"
                            L"Automatische Verstärkung und Klangverbesserungen des Mikrofons müssen in Windows ausgeschaltet sein.",
                            L"Kalibrieren", MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) start_job(1);
            return 0;
        case IDC_BROWSE: {
            BROWSEINFOW bi = { h, NULL, NULL, L"Ausgabeordner wählen", BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE, NULL, 0, 0 };
            LPITEMIDLIST id = SHBrowseForFolderW(&bi);
            if (id) { wchar_t p[MAX_PATH]; if (SHGetPathFromIDListW(id, p)) SetWindowTextW(g_out_e, p); CoTaskMemFree(id); }
            return 0; }
        case IDC_OPEN: { wchar_t p[700]; GetWindowTextW(g_out_e, p, 700); ShellExecuteW(NULL, L"explore", p, NULL, NULL, SW_SHOWNORMAL); return 0; }
        case IDC_RECAL: {
            if (g_running) return 0;
            double dl = edit_d(g_korr, 0);
            if (fabs(dl) < 0.05) { MessageBoxW(h, L"Bitte im Feld „Korrektur [dB]“ den Wert eintragen, um den alle vorhandenen Pegel verschoben werden sollen "
                                                  L"(z. B. +6 oder −3).", L"Laermtracker", MB_ICONINFORMATION); return 0; }
            wchar_t t[900];
            _snwprintf(t, 900, L"Alle vorhandenen Ereignisse im Ausgabeordner um %+.1f dB umrechnen?\n\nCSV-Dateien, Ereignis- und Tagesdiagramme werden neu erzeugt und die "
                       L"Störungen neu bewertet. Das Original bleibt als EVENTS_ALT.CSV erhalten.\n\nNur einmal anwenden: Dieselbe Korrektur gilt danach auch für neue "
                       L"Aufnahmen (das Feld bleibt stehen). Waren Aufnahmen schon mit diesem Wert aufgenommen, würde doppelt korrigiert.", dl);
            if (MessageBoxW(h, t, L"Pegel nachträglich korrigieren", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            wchar_t ow[600]; GetWindowTextW(g_out_e, ow, 600); w2u(ow, g_out, sizeof g_out);
            SetCursor(LoadCursorW(NULL, IDC_WAIT));
            static int16_t dummy[1024];
            nc_config_t cfg; nc_default_config(&cfg); cfg.fs = g_fs;
            nc_io_t io = { NULL, io_now, io_mkdir, io_open, io_write, io_seek, io_close, io_read, io_remove };
            g_sim = 0; nc_init(&g_nc, &cfg, &io, dummy, 1024);
            nc_recal_t r; int rc = nc_recalibrate(&g_nc, (float)dl, &r);
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            if (rc) MessageBoxW(h, rc == -1 ? L"Im Ausgabeordner wurde keine EVENTS.CSV gefunden." : L"In EVENTS.CSV stehen keine Ereignisse.", L"Laermtracker", MB_ICONWARNING);
            else { _snwprintf(t, 900, L"Fertig: %lu Ereignisse umgerechnet, %lu Ereignisdiagramme und %lu Tagesdiagramme neu erzeugt.\nOriginal: EVENTS_ALT.CSV",
                              (unsigned long)r.rows, (unsigned long)r.svgs, (unsigned long)r.days);
                   MessageBoxW(h, t, L"Pegel nachträglich korrigieren", MB_ICONINFORMATION); }
            refresh_status();
            return 0; }
        case IDC_DAYPLOT: {
            SYSTEMTIME st; GetLocalTime(&st); wchar_t rel[64], base[700], p[800];
            _snwprintf(rel, 64, L"\\%02d%02d%02d\\TAG.SVG", st.wYear % 100, st.wMonth, st.wDay);
            GetWindowTextW(g_out_e, base, 700); _snwprintf(p, 800, L"%ls%ls", base, rel);
            if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) MessageBoxW(h, L"Für heute gibt es noch kein Tagesdiagramm (noch kein Ereignis).", L"Laermtracker", MB_ICONINFORMATION);
            else ShellExecuteW(NULL, L"open", p, NULL, NULL, SW_SHOWNORMAL);
            return 0; }
        case IDC_CSV: {
            wchar_t base[700], p[800]; GetWindowTextW(g_out_e, base, 700); _snwprintf(p, 800, L"%ls\\EVENTS.CSV", base);
            if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) MessageBoxW(h, L"Es gibt noch keine Ereignisliste.", L"Laermtracker", MB_ICONINFORMATION);
            else ShellExecuteW(NULL, L"open", p, NULL, NULL, SW_SHOWNORMAL);
            return 0; }
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)l)->idFrom == IDC_LIST && ((NMHDR *)l)->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)l;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) { if (cd->nmcd.lItemlParam) cd->clrText = RGB(190, 20, 20); return CDRF_DODEFAULT; }
        }
        break;
    case WM_APP_EVENT: add_event_row((wchar_t *)l); free((void *)l); return 0;
    case WM_APP_CAL:
        if (g_cal_result > -999) {
            set_edit_d(g_sens, g_cal_result, 1);
            wchar_t t[200]; _snwprintf(t, 200, L"Empfindlichkeit %.1f dBFS bei 94 dB übernommen.", g_cal_result); MessageBoxW(h, t, L"Kalibrierung", MB_ICONINFORMATION);
        } else MessageBoxW(h, L"Keine Daten empfangen.", L"Kalibrierung", MB_ICONERROR);
        return 0;
    case WM_APP_DONE:
        if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = NULL; }
        set_controls_enabled(0); refresh_status();
        if (g_selftest) {                                              /* Selbsttest: Ergebnis schreiben, beenden */
            int n = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
            char p[700]; snprintf(p, sizeof p, "%s/selftest_result.txt", g_out); wchar_t wp[700]; u2w(p, wp, 700);
            FILE *f = _wfopen(wp, L"wb"); if (f) { fprintf(f, "%s events_in_list=%d saved=%lu\n", n == 3 ? "OK" : "FAIL", n, (unsigned long)g_nc.events_total); fclose(f); }
            PostQuitMessage(n == 3 ? 0 : 1);
        }
        return 0;
    case WM_APP_SELFTEST:
        { wchar_t t[600]; GetWindowTextW(g_out_e, t, 600); (void)t; }
        start_job(2); return 0;
    case WM_CLOSE:
        stop_job(); save_settings(); DestroyWindow(h); return 0;
    case WM_DESTROY: KillTimer(h, IDC_TIMER); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hp; (void)cmd;
    CoInitialize(NULL);
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&ic);
    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t outarg[600] = L"", start[64] = L"";
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--selftest") && i + 1 < argc) { g_selftest = 1; wcsncpy(g_wavin, argv[++i], 519); g_mode = 2; g_sim = 1; }
        else if (!wcscmp(argv[i], L"--out") && i + 1 < argc) wcsncpy(outarg, argv[++i], 599);
        else if (!wcscmp(argv[i], L"--start") && i + 1 < argc) wcsncpy(start, argv[++i], 63);
    }
    if (g_selftest) {
        int y, mo, d, h, mi, s;
        if (swscanf(start, L"%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) g_sim_start_ms = nc_ms_from_civil(y, mo, d, h, mi, s, 0);
        uint32_t r = 0, n = 0; int16_t *w = read_wav(g_wavin, &r, &n); if (w) { g_fs = r; free(w); }
    }
    WNDCLASSEXW wc = { sizeof wc, CS_HREDRAW | CS_VREDRAW, wndproc, 0, 0, hi, LoadIconW(NULL, IDI_APPLICATION), LoadCursorW(NULL, IDC_ARROW), (HBRUSH)(COLOR_BTNFACE + 1), NULL, L"LaermtrackerMain", NULL };
    RegisterClassExW(&wc);
    WNDCLASSEXW gc = { sizeof gc, 0, graph_proc, 0, 0, hi, NULL, LoadCursorW(NULL, IDC_ARROW), NULL, NULL, L"LevelGraph", NULL };
    RegisterClassExW(&gc);
    HWND h = CreateWindowExW(0, L"LaermtrackerMain", L"Laermtracker – Lärmereignisse erkennen und dokumentieren", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 940, 660, NULL, NULL, hi, NULL);
    if (!h) return 2;
    if (outarg[0]) SetWindowTextW(g_out_e, outarg);
    if (g_selftest) PostMessageW(h, WM_APP_SELFTEST, 0, 0);
    ShowWindow(h, g_selftest ? SW_SHOWNOACTIVATE : show);
    MSG msg; while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)msg.wParam;
}
