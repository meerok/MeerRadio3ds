/*
 * Meer Radio - AM/FM internet radio for Nintendo 3DS
 *
 * Controls
 *   D-Pad L/R ....... tune to previous / next station
 *   D-Pad U/D ....... move through the station list
 *   A ............... play selected station (again = stop)
 *   B ............... stop
 *   X ............... favourite on/off
 *   Y ............... switch band  FM > AM > FAV
 *   L / R ........... volume down / up
 *   SELECT .......... screens off (any button turns them back on)
 *   START ........... exit
 *   Touch ........... tap stations, tabs, buttons; drag to scroll
 *   Country button .. opens the country picker (picking one rescans)
 */
#include <3ds.h>
#include <citro2d.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "countries.h"
#include "player.h"
#include "stations.h"
#include "ui.h"

#define SOC_BUFSIZE 0x100000

#define LIST_Y0 26
#define LIST_Y1 190
#define ROW_H   24
#define TB_Y    195
#define TB_W    58
#define TB_H    42

static C3D_RenderTarget *top, *bot;

/* --- app state ---------------------------------------------------------- */
static int   view[MAX_STATIONS];
static int   nview = 0;
static int   vband = 0;          /* 0 FM, 1 AM, 2 FAV */
static int   sel = 0;
static int   cur = -1;           /* tuned station (index into g_st) */
static float scroll = 0, scroll_vel = 0;
static bool  follow = true;

static float tsec = 0;
static float needle = 0.5f;
static int   needle_band = -1;
static float name_anim = 1;
static float vis[VIS_BANDS], peak[VIS_BANDS], peak_hold[VIS_BANDS];

static char  toast[80];
static float toast_t = 0;

static bool  audio_ok = false, net_ok = false;
static bool  screens_off = false, lid_off = false;
static bool  sleep_allowed = true;

/* touch */
enum { T_NONE, T_TAB0, T_TAB1, T_TAB2, T_COUNTRY, T_LIST, T_BTN0, T_BTN1, T_BTN2, T_BTN3, T_BTN4 };
static int   t_target = T_NONE;
static bool  t_drag = false;
static int   t_start_y = 0, t_last_y = 0;
static touchPosition t_last;

/* country picker */
static bool  picker_open = false;
static int   pk_sel = 0;
static float pk_scroll = 0, pk_vel = 0;
static bool  pk_drag = false, pk_touch = false;
static int   pk_start_y = 0, pk_last_y = 0;
static touchPosition pk_last;
#define PK_Y0  44
#define PK_Y1  206
#define PK_ROW 20

/* HOME Menu / sleep */
static aptHookCookie apt_cookie;
static volatile bool resume_wanted = false, was_playing = false, woke_up = false;
static bool  headphones = false;
static bool  sel_armed = false;   /* SELECT pressed on its own (not part of Luma's L+Down+SELECT) */

/* ------------------------------------------------------------------ log */
/* A small log on the SD card, so a crash report can say what was going on */
#define LOG_FILE APP_DIR "/log.txt"
static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t) fprintf(f, "%02d:%02d:%02d ", t->tm_hour, t->tm_min, t->tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
static void log_reset(void) {
    FILE *f = fopen(LOG_FILE, "r");
    long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
    if (sz > 64 * 1024) remove(LOG_FILE);
}

/* ------------------------------------------------------------------------ */

static void show_toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast, sizeof toast, fmt, ap);
    va_end(ap);
    toast_t = 2.2f;
}

static int cmp_view(const void *a, const void *b) {
    const Station *x = &g_st[*(const int *)a], *y = &g_st[*(const int *)b];
    if (x->band != y->band) return (int)x->band - (int)y->band;
    if (x->freq < y->freq) return -1;
    if (x->freq > y->freq) return 1;
    return strcasecmp(x->name, y->name);
}

static void rebuild_view(void) {
    int keep = (sel >= 0 && sel < nview) ? view[sel] : -1;
    nview = 0;
    for (int i = 0; i < g_nst; i++) {
        bool in = vband == 2 ? g_st[i].fav : ((int)g_st[i].band == vband && station_shows(i));
        if (in) view[nview++] = i;
    }
    qsort(view, (size_t)nview, sizeof(int), cmp_view);
    sel = 0;
    bool found = false;
    for (int j = 0; j < nview; j++) if (view[j] == keep) { sel = j; found = true; break; }
    if (!found) for (int j = 0; j < nview; j++) if (view[j] == cur) { sel = j; break; }
    follow = true;
}

static void freq_str(const Station *s, char *out, size_t n) {
    if (s->band == BAND_FM) snprintf(out, n, "%5.1f", s->freq);
    else snprintf(out, n, "%4d", (int)(s->freq + 0.5f));
}

static void tune_to(int idx) {
    if (idx < 0 || idx >= g_nst) return;
    if (!audio_ok) { show_toast("AUDIO NOT AVAILABLE"); return; }
    cur = idx;
    logf_("tune %s %.1f %s", g_st[idx].band == BAND_FM ? "FM" : "AM", g_st[idx].freq, g_st[idx].url);
    player_play(g_st[idx].url);
    snprintf(g_set.last, sizeof g_set.last, "%s", g_st[idx].url);
    settings_save();
    name_anim = 0;
}

static void tune_step(int dir) {
    if (nview == 0) return;
    int pos = -1;
    for (int j = 0; j < nview; j++) if (view[j] == cur) { pos = j; break; }
    int np = pos < 0 ? (dir > 0 ? 0 : nview - 1) : (pos + dir + nview) % nview;
    sel = np;
    follow = true;
    tune_to(view[np]);
}

static bool is_playing(void) {
    PlayerState s = player_state();
    return s == PS_PLAYING || s == PS_BUFFERING || s == PS_CONNECTING;
}

static void toggle_play(void) {
    if (is_playing()) { player_stop(); return; }
    if (nview && sel < nview) tune_to(view[sel]);
    else if (cur >= 0) tune_to(cur);
}

static void set_band(int b) {
    vband = (b + 3) % 3;
    rebuild_view();
    scroll = 0;
}

static void toggle_fav(void) {
    int idx = cur >= 0 ? cur : (nview ? view[sel] : -1);
    if (idx < 0) return;
    station_toggle_fav(idx);
    show_toast(g_st[idx].fav ? "ADDED TO FAVORITES" : "REMOVED FROM FAVORITES");
    if (vband == 2) rebuild_view();
}

static void change_volume(float d) {
    g_set.volume += d;
    if (g_set.volume < 0) g_set.volume = 0;
    if (g_set.volume > 1) g_set.volume = 1;
    player_set_volume(g_set.volume);
    show_toast("VOLUME %d%%", (int)(g_set.volume * 100 + 0.5f));
}

static void start_scan(void) {
    if (scan_state() == SCAN_RUNNING) return;
    if (!net_ok) { show_toast("NO NETWORK"); return; }
    logf_("scan %s", g_set.country);
    scan_start();
}

/* switching country throws away the old scan and loads the new one */
static void set_country(const char *code) {
    if (!strcasecmp(code, g_set.country) && scan_has_cache()) return;
    snprintf(g_set.country, sizeof g_set.country, "%s", code);
    settings_save();
    rebuild_view();
    if (scan_state() == SCAN_RUNNING) return;
    char keep[384];
    snprintf(keep, sizeof keep, "%s", cur >= 0 ? g_st[cur].url : "");
    scan_clear();
    cur = station_find_url(keep);
    rebuild_view();
    scroll = 0;
    start_scan();
}

static void open_picker(void) {
    picker_open = true;
    pk_sel = country_index(g_set.country);
    if (pk_sel < 0) pk_sel = 0;
    float area = PK_Y1 - PK_Y0;
    pk_scroll = pk_sel * PK_ROW - area / 2 + PK_ROW / 2;
    pk_vel = 0;
    pk_touch = false;
}

/* Runs inside aptMainLoop when HOME is pressed or the console sleeps. It only
 * flips flags: taking a lock here can deadlock against the audio threads and
 * freeze the console. The main loop restarts the station afterwards. */
static void apt_hook(APT_HookType hook, void *param) {
    (void)param;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            was_playing = is_playing() && cur >= 0;
            player_set_suspended(true);
            logf_(hook == APTHOOK_ONSLEEP ? "going to sleep" : "HOME pressed, pausing");
            break;
        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            player_set_suspended(false);
            logf_(hook == APTHOOK_ONWAKEUP ? "woke up" : "back from HOME");
            if (was_playing) resume_wanted = true;
            if (hook == APTHOOK_ONWAKEUP) woke_up = true;
            was_playing = false;
            break;
        default:
            break;
    }
}

/* The LCD service is opened only for the moment it's needed: holding it open
 * the whole time can stop the HOME Menu from getting it. */
static void set_screens(bool on) {
    if (R_FAILED(gspLcdInit())) return;
    if (on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    else GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH);
    gspLcdExit();
}

/* ================================================================ TOP === */

static void lcd_number(float x, float y, const char *s, int sc, u32 on, u32 ghost) {
    float px = x;
    for (; *s; s++) {
        if (*s == '.') {
            ui_rect(px + sc * 0.5f, y + 6 * sc, sc, sc, on);
            px += 2 * sc;
            continue;
        }
        ui_text(px, y, "8", sc, ghost);
        if (*s != ' ') {
            char c[2] = { *s, 0 };
            ui_text(px, y, c, sc, on);
        }
        px += 6 * sc;
    }
}

static float lcd_number_w(const char *s, int sc) {
    float w = 0;
    for (; *s; s++) w += (*s == '.') ? 2 * sc : 6 * sc;
    return w - sc;
}

static void draw_statusbar(void) {
    ui_text_shadow(12, 8, "MEER", 1, C_YELLOW, C_BLACK);
    ui_text_shadow(40, 8, "RADIO", 1, C_WHITE, C_BLACK);

    /* volume */
    ui_icon(IC_SPEAKER, 216, 6, 1, C_LGRAY, false);
    ui_rect(230, 9, 52, 5, C_BLACK);
    ui_rect(231, 10, 50 * g_set.volume, 3, C_GREEN);

    /* wifi */
    u8 wf = osGetWifiStrength();
    for (int i = 0; i < 3; i++) {
        float h = 3 + i * 3;
        ui_rect(292 + i * 4, 15 - h, 3, h, (i < wf) ? C_WHITE : C_GRAY);
    }

    /* clock */
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    char clk[8];
    snprintf(clk, sizeof clk, "%02d:%02d", tmv ? tmv->tm_hour : 0, tmv ? tmv->tm_min : 0);
    ui_text_shadow(310, 8, clk, 1, C_WHITE, C_BLACK);

    /* battery */
    u8 lvl = 0, chg = 0;
    PTMU_GetBatteryLevel(&lvl);
    PTMU_GetBatteryChargeState(&chg);
    ui_frame(350, 7, 22, 9, 1, C_WHITE);
    ui_rect(372, 9, 2, 5, C_WHITE);
    u32 bc = lvl <= 1 ? C_RED : (chg ? C_YELLOW : C_LGREEN);
    for (int i = 0; i < 5 && i < lvl; i++) ui_rect(352 + i * 4, 9, 3, 5, bc);
    if (chg && ((int)(tsec * 2) & 1)) ui_rect(352 + (lvl < 5 ? lvl : 4) * 4, 9, 3, 5, C_WHITE);
}

static void draw_lcd(void) {
    const float X = 10, Y = 22, W = 380, H = 100;
    ui_rect(X, Y, W, H, C_BLACK);
    ui_rect(X + 2, Y + 2, W - 4, H - 4, LCD_BG);
    ui_rect(X + 2, Y + 2, W - 4, 1, RGB(30, 36, 44));

    PlayerState ps = player_state();
    const Station *st = cur >= 0 ? &g_st[cur] : NULL;
    Band band = st ? st->band : (vband == 1 ? BAND_AM : BAND_FM);

    /* band chip */
    ui_rect(20, 30, 34, 18, C_YELLOW);
    ui_text(26, 32, band == BAND_FM ? "FM" : "AM", 2, LCD_BG);

    bool live = ps == PS_PLAYING;
    int ch = player_channels();
    ui_text(20, 54, (live && ch == 2) ? "STEREO" : (live && ch == 1 ? "MONO" : "------"), 1, live ? C_YELLOW : LCD_DIM);
    char br[16];
    int kb = player_bitrate();
    if (live && kb > 0) snprintf(br, sizeof br, "%dK", kb); else snprintf(br, sizeof br, "---K");
    ui_text(20, 64, br, 1, live ? C_YELLOW : LCD_DIM);

    /* frequency */
    char fs[12];
    if (st) freq_str(st, fs, sizeof fs);
    else snprintf(fs, sizeof fs, band == BAND_FM ? "  -.-" : "   -");
    const int SC = 6;
    float fw = lcd_number_w(band == BAND_FM ? "888.8" : "8888", SC);
    lcd_number(296 - fw, 30, fs, SC, C_YELLOW, LCD_GHOST);
    ui_text(304, 60, band == BAND_FM ? "MHz" : "kHz", 2, C_YELLOW);

    /* status */
    char stt[24];
    u32 sc = C_YELLOW;
    bool blink = ((int)(tsec * 3) & 1);
    switch (ps) {
        case PS_PLAYING:    snprintf(stt, sizeof stt, "LIVE"); sc = C_LGREEN; break;
        case PS_CONNECTING: snprintf(stt, sizeof stt, "TUNING"); break;
        case PS_BUFFERING:  snprintf(stt, sizeof stt, "BUF %d%%", player_buffer_pct()); break;
        case PS_ERROR:      snprintf(stt, sizeof stt, "NO SIGNAL"); sc = C_RED; break;
        default:            snprintf(stt, sizeof stt, "STANDBY"); sc = LCD_DIM; break;
    }
    if (ps == PS_PLAYING || (ps != PS_STOPPED && blink)) ui_rect(304, 33, 4, 4, sc);
    ui_text(311, 32, stt, 1, sc);

    /* favourite */
    if (st) ui_icon(IC_HEART, 368, 44, 1, st->fav ? C_RED : LCD_GHOST, false);

    ui_rect(18, 80, 364, 1, LCD_GHOST);

    /* station name, slides in on change */
    float a = name_anim < 1 ? name_anim : 1;
    float ease = 1 - (1 - a) * (1 - a);
    const char *nm = st ? st->name : (g_nst ? "SELECT A STATION" : "NO STATIONS - PRESS SCAN");
    ui_text_clip(20 + (1 - ease) * 24, 86, nm, 2, ui_alpha(C_YELLOW, ease), 18, 382);

    /* now playing / error marquee */
    char line[300];
    u32 lc = C_YELLOW;
    if (!audio_ok) { snprintf(line, sizeof line, "AUDIO OFF: dump DSP firmware (sdmc:/3ds/dspfirm.cdc)"); lc = C_RED; }
    else if (!net_ok) { snprintf(line, sizeof line, "NO NETWORK - check Wi-Fi"); lc = C_RED; }
    else if (ps == PS_ERROR) { player_get_error(line, sizeof line); lc = C_RED; }
    else if (ps == PS_PLAYING) {
        char raw[256];
        player_get_title(raw, sizeof raw);
        fold_ascii(raw, line, sizeof line);
        if (!line[0]) snprintf(line, sizeof line, "On air");
    } else if (ps == PS_CONNECTING) snprintf(line, sizeof line, "Searching for signal...");
    else if (ps == PS_BUFFERING) snprintf(line, sizeof line, "Locking on...");
    else snprintf(line, sizeof line, "Press A to listen");

    ui_icon(IC_NOTE, 20, 104, 1, lc == C_RED ? C_RED : LCD_DIM, false);
    const float mx0 = 36, mx1 = 382;
    int tw = ui_text_w(line, 1);
    if (tw <= mx1 - mx0) {
        ui_text(mx0, 106, line, 1, lc);
    } else {
        float span = (float)tw + 60;
        float off = fmodf(tsec * 28.0f, span);
        ui_text_clip(mx0 - off, 106, line, 1, lc, mx0, mx1);
        ui_text_clip(mx0 - off + span, 106, line, 1, lc, mx0, mx1);
    }

    /* LCD scanlines */
    for (float yy = Y + 3; yy < Y + H - 2; yy += 3) ui_rect(X + 2, yy, W - 4, 1, RGBA(0, 0, 0, 46));
}

static void band_range(Band b, float *lo, float *hi) {
    if (b == BAND_FM) { *lo = 87.5f; *hi = 108.0f; }
    else { *lo = 530; *hi = 1710; }
}

static void draw_dial(float dt) {
    const float X = 10, Y = 128, W = 380, H = 46;
    const float x0 = 24, x1 = 376;
    ui_rect(X, Y, W, H, C_BLACK);
    ui_rect(X + 2, Y + 2, W - 4, H - 4, RGB(20, 24, 40));
    ui_rect(X + 2, Y + 2, W - 4, 14, RGBA(41, 54, 111, 90));

    Band band = cur >= 0 ? g_st[cur].band : (vband == 1 ? BAND_AM : BAND_FM);
    float lo, hi;
    band_range(band, &lo, &hi);
#define FX(f) (x0 + ((f) - lo) / (hi - lo) * (x1 - x0))

    if (band == BAND_FM) {
        for (float f = 88.0f; f <= 108.01f; f += 0.5f) {
            int fi = (int)roundf(f * 2);
            bool major = (fi % 4) == 0, mid = (fi % 2) == 0;
            float h = major ? 10 : (mid ? 7 : 4);
            ui_rect(FX(f), Y + 30 - h, 1, h, major ? C_LGRAY : C_GRAY);
            if (major) {
                char l[8];
                snprintf(l, sizeof l, "%d", (int)roundf(f));
                ui_text_center(FX(f) + 0.5f, Y + 5, l, 1, C_LGRAY);
            }
        }
    } else {
        for (int f = 550; f <= 1700; f += 50) {
            bool major = (f % 200) == 0, mid = (f % 100) == 0;
            float h = major ? 10 : (mid ? 7 : 4);
            ui_rect(FX(f), Y + 30 - h, 1, h, major ? C_LGRAY : C_GRAY);
            if (major) {
                char l[8];
                snprintf(l, sizeof l, "%d", f);
                ui_text_center(FX(f) + 0.5f, Y + 5, l, 1, C_LGRAY);
            }
        }
    }

    /* station markers */
    for (int i = 0; i < g_nst; i++) {
        if (g_st[i].band != band || !station_shows(i)) continue;
        float sx = FX(g_st[i].freq);
        ui_rect(sx - 1, Y + 35, 2, 3, g_st[i].fav ? C_RED : C_SKY);
    }

    /* needle */
    float target = needle;
    if (cur >= 0) target = (g_st[cur].freq - lo) / (hi - lo);
    if (needle_band != (int)band) { needle = target; needle_band = (int)band; }
    float k = dt * 7.0f;
    if (k > 1) k = 1;
    needle += (target - needle) * k;
    float nx = x0 + needle * (x1 - x0);
    ui_rect(nx - 2, Y + 3, 5, H - 6, RGBA(239, 125, 87, 60));
    ui_rect(nx, Y + 2, 2, H - 4, C_RED);
    ui_rect(nx - 2, Y + 2, 6, 2, C_ORANGE);
#undef FX
}

static void draw_grille_vis(float dt) {
    float lv[VIS_BANDS];
    player_get_levels(lv);
    float energy = 0;
    for (int i = 0; i < VIS_BANDS; i++) {
        float t = lv[i];
        if (t > vis[i]) vis[i] += (t - vis[i]) * (dt * 30 > 1 ? 1 : dt * 30);
        else vis[i] -= dt * 1.6f;
        if (vis[i] < 0) vis[i] = 0;
        if (vis[i] > peak[i]) { peak[i] = vis[i]; peak_hold[i] = 0.5f; }
        else if ((peak_hold[i] -= dt) < 0) peak[i] -= dt * 0.8f;
        if (peak[i] < 0) peak[i] = 0;
        if (i < 4) energy += vis[i] * 0.25f;
    }

    /* speaker grille, pulses with the bass */
    const float GX = 10, GY = 180, GW = 190, GH = 56;
    ui_rect(GX, GY, GW, GH, C_BLACK);
    ui_rect(GX + 2, GY + 2, GW - 4, GH - 4, RGB(36, 42, 62));
    u32 dot = ui_mix(C_DGRAY, C_GRAY, energy * 1.4f);
    for (float y = GY + 6; y < GY + GH - 5; y += 5)
        for (float x = GX + 6; x < GX + GW - 5; x += 5)
            ui_rect(x, y, 2, 2, dot);
    /* badge */
    ui_rect(GX + GW / 2 - 30, GY + GH / 2 - 7, 60, 14, C_DGRAY);
    ui_frame(GX + GW / 2 - 30, GY + GH / 2 - 7, 60, 14, 1, C_GRAY);
    ui_text_center(GX + GW / 2, GY + GH / 2 - 3, "MEER-1", 1, C_YELLOW);

    /* spectrum */
    const float VX = 206, VY = 180, VW = 184, VH = 56;
    ui_rect(VX, VY, VW, VH, C_BLACK);
    ui_rect(VX + 2, VY + 2, VW - 4, VH - 4, LCD_BG);
    const int SEGS = 12;
    for (int b = 0; b < VIS_BANDS; b++) {
        float bx = VX + 6 + b * 11;
        int on = (int)(vis[b] * SEGS + 0.5f);
        for (int s = 0; s < SEGS; s++) {
            float sy = VY + VH - 6 - (s + 1) * 4;
            u32 c = s < 7 ? C_GREEN : (s < 10 ? C_YELLOW : C_RED);
            ui_rect(bx, sy, 9, 3, s < on ? c : LCD_GHOST);
        }
        int pk = (int)(peak[b] * SEGS + 0.5f);
        if (pk > 0) ui_rect(bx, VY + VH - 6 - pk * 4, 9, 1, C_WHITE);
    }
}

static void draw_toast(void) {
    if (toast_t <= 0) return;
    float a = toast_t > 0.3f ? 1 : toast_t / 0.3f;
    int tw = ui_text_w(toast, 1);
    float w = (float)tw + 20, x = 200 - w / 2, y = 143;
    ui_rect(x, y, w, 17, ui_alpha(C_BLACK, a * 0.92f));
    ui_frame(x, y, w, 17, 1, ui_alpha(C_YELLOW, a));
    ui_text(x + 10, y + 5, toast, 1, ui_alpha(C_WHITE, a));
}

static void draw_top(float dt) {
    ui_rect(0, 0, 400, 240, C_DGRAY);
    ui_rect(0, 0, 400, 2, C_GRAY);
    ui_rect(0, 0, 2, 240, C_GRAY);
    ui_rect(0, 238, 400, 2, C_BLACK);
    ui_rect(398, 0, 2, 240, C_BLACK);
    draw_statusbar();
    draw_lcd();
    draw_dial(dt);
    draw_grille_vis(dt);
    draw_toast();
}

/* ============================================================= BOTTOM === */

static float max_scroll(void) {
    float m = (float)(nview * ROW_H) - (LIST_Y1 - LIST_Y0);
    return m > 0 ? m : 0;
}

static void mini_eq(float x, float y, u32 c) {
    for (int i = 0; i < 3; i++) {
        float h = 3 + 5 * fabsf(sinf(tsec * (5.0f + i * 1.7f) + i));
        ui_rect(x + i * 3, y + 9 - h, 2, h, c);
    }
}

static void draw_list(void) {
    PlayerState ps = player_state();
    if (nview == 0) {
        const char *a = vband == 2 ? "NO FAVORITES YET" : "NO STATIONS HERE";
        const char *b = vband == 2 ? "PRESS X ON A STATION TO ADD IT" : "TAP SCAN TO FIND AM/FM STATIONS";
        ui_icon(vband == 2 ? IC_HEART : IC_SCAN, 160 - 11, 70, 2, C_GRAY, false);
        ui_text_center(160, 102, a, 2, C_LGRAY);
        ui_text_center(160, 122, b, 1, C_GRAY);
        return;
    }
    for (int j = 0; j < nview; j++) {
        float y = LIST_Y0 + j * ROW_H - scroll;
        if (y + ROW_H < LIST_Y0 || y > LIST_Y1) continue;
        const Station *s = &g_st[view[j]];
        bool is_sel = j == sel, is_cur = view[j] == cur;
        ui_rect(0, y, 316, ROW_H, is_sel ? C_DGRAY : ((j & 1) ? RGB(31, 34, 54) : C_BLACK));
        if (is_sel) ui_rect(0, y, 3, ROW_H, C_YELLOW);

        char fs[12];
        freq_str(s, fs, sizeof fs);
        ui_text(8, y + 5, fs, 2, is_cur ? C_YELLOW : C_ORANGE);
        ui_text_clip(76, y + 4, s->name, 1, is_sel ? C_WHITE : C_LGRAY, 76, 274);

        char sub[120];
        u32 subc = C_GRAY;
        if (is_cur && ps != PS_STOPPED) {
            if (ps == PS_PLAYING) {
                char raw[200];
                player_get_title(raw, sizeof raw);
                fold_ascii(raw, sub, sizeof sub);
                if (!sub[0]) snprintf(sub, sizeof sub, "ON AIR");
                subc = C_SKY;
            } else if (ps == PS_ERROR) { snprintf(sub, sizeof sub, "NO SIGNAL"); subc = C_RED; }
            else { snprintf(sub, sizeof sub, "TUNING..."); subc = C_YELLOW; }
        } else {
            snprintf(sub, sizeof sub, "%s %s", s->band == BAND_FM ? "MHZ" : "KHZ", s->scanned ? "- SCANNED" : "- PRESET");
        }
        ui_text_clip(76, y + 14, sub, 1, subc, 76, 274);

        if (is_cur && ps == PS_PLAYING) mini_eq(280, y + 6, C_LGREEN);
        if (s->fav) ui_icon(IC_HEART, 294, y + 6, 1, C_RED, false);
        ui_rect(0, y + ROW_H - 1, 316, 1, RGBA(86, 108, 134, 50));
    }
    /* scrollbar */
    float ms = max_scroll();
    if (ms > 0) {
        float area = LIST_Y1 - LIST_Y0;
        float th = area * area / (nview * ROW_H);
        if (th < 12) th = 12;
        float ty = LIST_Y0 + (scroll / ms) * (area - th);
        ui_rect(316, LIST_Y0, 4, area, C_BLACK);
        ui_rect(317, ty, 2, th, C_GRAY);
    }
}

static void draw_header(void) {
    static const char *tabs[3] = { "FM", "AM", "FAV" };
    ui_rect(0, 0, 320, LIST_Y0 - 2, C_NAVY);
    ui_rect(0, LIST_Y0 - 2, 320, 2, C_BLUE);
    for (int i = 0; i < 3; i++) {
        float x = 4 + i * 52, y = 3;
        bool act = vband == i;
        bool pr = t_target == T_TAB0 + i;
        if (act) ui_bevel(x, y + (pr ? 1 : 0), 48, 19, C_YELLOW, C_WHITE, C_ORANGE);
        else ui_bevel(x, y + (pr ? 1 : 0), 48, 19, C_DGRAY, C_GRAY, C_BLACK);
        ui_text_center(x + 24, y + 3 + (pr ? 1 : 0), tabs[i], 2, act ? C_BLACK : C_LGRAY);
    }
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%d ST", nview);
    ui_text(166, 11, cnt, 1, C_LGRAY);

    bool pr = t_target == T_COUNTRY;
    ui_bevel(262, 3 + (pr ? 1 : 0), 54, 19, C_TEAL, C_CYAN, C_BLACK);
    ui_text(268, 9 + (pr ? 1 : 0), "@", 1, C_CYAN);
    ui_text(280, 6 + (pr ? 1 : 0), g_set.country, 2, C_WHITE);
}

static void tb_rect(int i, float *x, float *y) {
    *x = 5 + i * 62;
    *y = TB_Y;
}

static void draw_toolbar(void) {
    ui_rect(0, LIST_Y1 + 1, 320, 240 - LIST_Y1 - 1, C_DGRAY);
    ui_rect(0, LIST_Y1 + 1, 320, 1, C_GRAY);
    bool playing = is_playing();
    const char *labels[5] = { "SCAN", "PREV", playing ? "STOP" : "PLAY", "NEXT", "FAV" };
    for (int i = 0; i < 5; i++) {
        float x, y;
        tb_rect(i, &x, &y);
        bool pr = t_target == T_BTN0 + i;
        float oy = pr ? 2 : 0;
        u32 face = C_NAVY, hi = C_BLUE, lo = C_BLACK;
        if (i == 2) {
            if (playing) { face = C_RED; hi = C_ORANGE; }
            else { face = C_GREEN; hi = C_LGREEN; }
        }
        if (pr) { u32 t = hi; hi = lo; lo = t; }
        ui_rect(x, y + TB_H - 2, TB_W, 2, C_BLACK);
        ui_bevel(x, y + oy, TB_W, TB_H - 2, face, hi, lo);
        Icon ic = IC_SCAN;
        bool mir = false;
        u32 icc = C_WHITE;
        switch (i) {
            case 0: ic = IC_SCAN; if (scan_state() == SCAN_RUNNING && ((int)(tsec * 4) & 1)) icc = C_YELLOW; break;
            case 1: ic = IC_PREV; break;
            case 2: ic = playing ? IC_STOP : IC_PLAY; break;
            case 3: ic = IC_PREV; mir = true; break;
            case 4: ic = IC_HEART; icc = (cur >= 0 && g_st[cur].fav) ? C_RED : C_LGRAY; break;
        }
        ui_icon(ic, x + TB_W / 2 - 11, y + 4 + oy, 2, icc, mir);
        ui_text_center(x + TB_W / 2, y + 29 + oy, labels[i], 1, C_WHITE);
    }
}

static void draw_scan_overlay(void) {
    ui_rect(0, 0, 320, 240, RGBA(10, 12, 20, 200));
    ui_bevel(40, 66, 240, 108, C_NAVY, C_BLUE, C_BLACK);
    ui_frame(44, 70, 232, 100, 1, C_SKY);
    ui_icon(IC_SCAN, 60, 86, 2, C_YELLOW, false);
    ui_text(92, 88, "SCANNING", 2, C_WHITE);
    char l[48];
    snprintf(l, sizeof l, "%s", country_name(g_set.country));
    ui_text(92, 106, l, 1, C_LGRAY);
    /* spinner */
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f + tsec * 5;
        float px = 160 + cosf(a) * 10, py = 140 + sinf(a) * 10;
        float t = fmodf((float)i / 8.0f + tsec * 0.8f, 1.0f);
        ui_rect(px - 1.5f, py - 1.5f, 3, 3, ui_mix(C_DGRAY, C_YELLOW, t));
    }
    snprintf(l, sizeof l, "FOUND %d", scan_found());
    ui_text_center(160, 156, l, 1, C_YELLOW);
    ui_text_clip(52, 118, scan_msg(), 1, C_SKY, 52, 270);
}

static float pk_max(void) {
    float m = (float)(COUNTRY_COUNT * PK_ROW) - (PK_Y1 - PK_Y0);
    return m > 0 ? m : 0;
}

static void draw_picker(void) {
    ui_rect(0, 0, 320, 240, C_BLACK);
    for (int i = 0; i < COUNTRY_COUNT; i++) {
        float y = PK_Y0 + i * PK_ROW - pk_scroll;
        if (y + PK_ROW < PK_Y0 || y > PK_Y1) continue;
        bool is_sel = i == pk_sel, is_cur = !strcasecmp(COUNTRY_LIST[i].code, g_set.country);
        ui_rect(0, y, 316, PK_ROW, is_sel ? C_DGRAY : ((i & 1) ? RGB(31, 34, 54) : C_BLACK));
        if (is_sel) ui_rect(0, y, 3, PK_ROW, C_YELLOW);
        ui_text(10, y + 4, COUNTRY_LIST[i].code, 2, is_cur ? C_YELLOW : C_ORANGE);
        ui_text(46, y + 7, COUNTRY_LIST[i].name, 1, is_sel ? C_WHITE : C_LGRAY);
        if (is_cur) ui_text(262, y + 7, "CURRENT", 1, C_GREEN);
    }
    float ms = pk_max();
    if (ms > 0) {
        float area = PK_Y1 - PK_Y0;
        float th = area * area / (COUNTRY_COUNT * PK_ROW);
        float ty = PK_Y0 + (pk_scroll / ms) * (area - th);
        ui_rect(316, PK_Y0, 4, area, C_BLACK);
        ui_rect(317, ty, 2, th, C_GRAY);
    }
    ui_rect(0, 0, 320, PK_Y0 - 2, C_NAVY);
    ui_rect(0, PK_Y0 - 2, 320, 2, C_BLUE);
    ui_text(8, 6, "PICK YOUR COUNTRY", 2, C_WHITE);
    ui_text(8, 26, "Picking one scans its AM/FM stations", 1, C_LGRAY);
    ui_rect(0, PK_Y1, 320, 240 - PK_Y1, C_DGRAY);
    ui_rect(0, PK_Y1, 320, 1, C_GRAY);
    ui_text(8, PK_Y1 + 13, "A/TAP: PICK", 1, C_WHITE);
    ui_text(110, PK_Y1 + 13, "B: CANCEL", 1, C_WHITE);
    ui_text(200, PK_Y1 + 13, "L/R: PAGE", 1, C_WHITE);
}

static void picker_input(u32 kd, u32 kh, u32 ku, u32 kr) {
    int page = (int)((PK_Y1 - PK_Y0) / PK_ROW);
    bool moved = false;
    if (kr & KEY_DOWN) { if (pk_sel < COUNTRY_COUNT - 1) pk_sel++; moved = true; }
    if (kr & KEY_UP) { if (pk_sel > 0) pk_sel--; moved = true; }
    if (kr & KEY_R) { pk_sel += page; if (pk_sel > COUNTRY_COUNT - 1) pk_sel = COUNTRY_COUNT - 1; moved = true; }
    if (kr & KEY_L) { pk_sel -= page; if (pk_sel < 0) pk_sel = 0; moved = true; }
    if (moved) pk_vel = 0;
    if (kd & KEY_B) { picker_open = false; return; }
    if (kd & KEY_A) { picker_open = false; set_country(COUNTRY_LIST[pk_sel].code); return; }

    touchPosition tp;
    hidTouchRead(&tp);
    if (kd & KEY_TOUCH) {
        pk_touch = tp.py >= PK_Y0 && tp.py < PK_Y1;
        pk_drag = false;
        pk_start_y = pk_last_y = tp.py;
        pk_last = tp;
        pk_vel = 0;
    } else if ((kh & KEY_TOUCH) && pk_touch) {
        int dy = tp.py - pk_last_y;
        if (!pk_drag && abs(tp.py - pk_start_y) > 6) pk_drag = true;
        if (pk_drag) { pk_scroll -= dy; pk_vel = (float)-dy; }
        pk_last_y = tp.py;
        pk_last = tp;
    } else if ((ku & KEY_TOUCH) && pk_touch) {
        pk_touch = false;
        if (!pk_drag) {
            int row = (int)((pk_last.py - PK_Y0 + pk_scroll) / PK_ROW);
            if (row >= 0 && row < COUNTRY_COUNT) {
                pk_sel = row;
                picker_open = false;
                set_country(COUNTRY_LIST[row].code);
                return;
            }
        }
    }

    if (!(kh & KEY_TOUCH)) {
        pk_scroll += pk_vel;
        pk_vel *= 0.9f;
        if (fabsf(pk_vel) < 0.05f) pk_vel = 0;
        if (moved) {
            float area = PK_Y1 - PK_Y0, sy = (float)pk_sel * PK_ROW;
            if (sy < pk_scroll) pk_scroll = sy;
            if (sy + PK_ROW > pk_scroll + area) pk_scroll = sy + PK_ROW - area;
        }
        float ms = pk_max();
        if (pk_scroll < 0) pk_scroll += (0 - pk_scroll) * 0.35f;
        if (pk_scroll > ms) pk_scroll += (ms - pk_scroll) * 0.35f;
    }
}

static void draw_bottom(void) {
    if (picker_open) { draw_picker(); return; }
    ui_rect(0, 0, 320, 240, C_BLACK);
    draw_list();
    draw_header();
    draw_toolbar();
    if (scan_state() == SCAN_RUNNING) draw_scan_overlay();
}

/* ============================================================== INPUT === */

static int hit_test(int x, int y) {
    if (y < LIST_Y0 - 2) {
        for (int i = 0; i < 3; i++) if (x >= 4 + i * 52 && x < 52 + i * 52) return T_TAB0 + i;
        if (x >= 262) return T_COUNTRY;
        return T_NONE;
    }
    if (y < LIST_Y1) return T_LIST;
    for (int i = 0; i < 5; i++) {
        float bx, by;
        tb_rect(i, &bx, &by);
        if (x >= bx && x < bx + TB_W && y >= by - 3) return T_BTN0 + i;
    }
    return T_NONE;
}

static void activate(int target, touchPosition tp) {
    switch (target) {
        case T_TAB0: case T_TAB1: case T_TAB2: set_band(target - T_TAB0); break;
        case T_COUNTRY: open_picker(); break;
        case T_LIST: {
            int row = (int)((tp.py - LIST_Y0 + scroll) / ROW_H);
            if (row >= 0 && row < nview) {
                if (view[row] == cur && is_playing()) player_stop();
                else { sel = row; tune_to(view[row]); }
            }
            break;
        }
        case T_BTN0: start_scan(); break;
        case T_BTN1: tune_step(-1); break;
        case T_BTN2: toggle_play(); break;
        case T_BTN3: tune_step(1); break;
        case T_BTN4: toggle_fav(); break;
    }
}

static void handle_touch(u32 kd, u32 kh, u32 ku) {
    touchPosition tp;
    hidTouchRead(&tp);
    if (kd & KEY_TOUCH) {
        t_target = hit_test(tp.px, tp.py);
        t_drag = false;
        t_start_y = t_last_y = tp.py;
        t_last = tp;
        scroll_vel = 0;
        if (t_target == T_LIST) follow = false;
    } else if (kh & KEY_TOUCH) {
        int dy = tp.py - t_last_y;
        if (t_target == T_LIST) {
            if (!t_drag && abs(tp.py - t_start_y) > 6) t_drag = true;
            if (t_drag) { scroll -= dy; scroll_vel = (float)-dy; }
        } else if (t_target >= T_BTN0 || t_target <= T_COUNTRY) {
            if (hit_test(tp.px, tp.py) != t_target) t_target = T_NONE;
        }
        t_last_y = tp.py;
        t_last = tp;
    } else if (ku & KEY_TOUCH) {
        if (t_target != T_NONE && !t_drag) activate(t_target, t_last);
        t_target = T_NONE;
    }
}

/* =============================================================== MAIN === */

int main(void) {
    osSetSpeedupEnable(true);
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(12000);
    C2D_Prepare();
    top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    ptmuInit();

    u32 *soc_buf = (u32 *)memalign(0x1000, SOC_BUFSIZE);
    net_ok = soc_buf && R_SUCCEEDED(socInit(soc_buf, SOC_BUFSIZE));

    ui_init();
    stations_init();
    log_reset();
    logf_("start (net %d)", net_ok ? 1 : 0);
    if (g_first_run) {
        const char *sys = country_from_system();
        if (sys) snprintf(g_set.country, sizeof g_set.country, "%s", sys);
        settings_save();
        logf_("first run, country %s", g_set.country);
    }
    audio_ok = player_init();
    if (!audio_ok) logf_("audio init failed");
    /* hooked after the sound system so ours runs first and pauses decoding
     * before the DSP is handed back to the HOME Menu */
    aptHook(&apt_cookie, apt_hook, NULL);
    player_set_volume(g_set.volume);
    player_set_static(g_set.static_on);

    cur = station_find_url(g_set.last);
    if (cur >= 0) vband = (int)g_st[cur].band;
    rebuild_view();

    if (!scan_has_cache() && net_ok) scan_start();
    else if (cur >= 0 && audio_ok && net_ok && osGetWifiStrength() > 0) tune_to(cur);

    hidSetRepeatParameters(18, 5);
    u64 last = osGetTime();
    float shell_t = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp(), kr = hidKeysDownRepeat();
        u64 now = osGetTime();
        float dt = (float)(now - last) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;
        tsec += dt;

        if (screens_off) {
            if (kd) { screens_off = false; if (!lid_off) set_screens(true); }
            kd = kr = 0;
        }
        if (kd & KEY_START) break;

        /* scan completion */
        ScanState ss = scan_state();
        if (ss == SCAN_DONE || ss == SCAN_FAILED) {
            char keep[384];
            snprintf(keep, sizeof keep, "%s", cur >= 0 ? g_st[cur].url : "");
            const char *msg = scan_msg();
            char m[96];
            snprintf(m, sizeof m, "%s", msg);
            bool ok = ss == SCAN_DONE;
            int n = scan_collect();
            cur = station_find_url(keep);
            rebuild_view();
            logf_("scan done: %s (%d added)", m, n);
            if (ok && n > 0) show_toast("FOUND %d STATIONS IN %s", n, g_set.country);
            else if (ok) show_toast("NO MP3 AM/FM STATIONS FOUND FOR %s", g_set.country);
            else show_toast("%s", m);
        }

        player_tick();
        if (resume_wanted) {
            resume_wanted = false;
            logf_("resume after HOME/sleep");
            if (cur >= 0) tune_to(cur);
            if (woke_up && !headphones) show_toast("PLUG IN HEADPHONES TO LISTEN WITH THE LID SHUT");
        }
        woke_up = false;

        /* SELECT = screens off, but only when pressed and released on its own,
         * so Luma's Rosalina shortcut (L + Down + SELECT) doesn't trigger it */
        if (kd & KEY_SELECT) sel_armed = (kh & ~(u32)(KEY_SELECT | KEY_TOUCH)) == 0;
        else if ((kh & KEY_SELECT) && (kh & ~(u32)(KEY_SELECT | KEY_TOUCH))) sel_armed = false;
        bool select_tap = sel_armed && (ku & KEY_SELECT);
        if (ku & KEY_SELECT) sel_armed = false;

        /* log state changes, errors included */
        static PlayerState last_ps = PS_STOPPED;
        PlayerState ps_now = player_state();
        if (ps_now != last_ps) {
            if (ps_now == PS_ERROR) {
                char e[128];
                player_get_error(e, sizeof e);
                logf_("error: %s", e);
            } else if (ps_now == PS_PLAYING) {
                logf_("playing (%d kbps, %d ch)", player_bitrate(), player_channels());
            }
            last_ps = ps_now;
        }

        if (picker_open && scan_state() != SCAN_RUNNING) {
            picker_input(kd, kh, ku, kr);
            if (select_tap) { screens_off = true; set_screens(false); }
        } else if (scan_state() != SCAN_RUNNING) {
            if (kr & KEY_DOWN) { if (sel < nview - 1) sel++; follow = true; }
            if (kr & KEY_UP) { if (sel > 0) sel--; follow = true; }
            if (kr & KEY_CPAD_DOWN) { if (sel < nview - 1) sel++; follow = true; }
            if (kr & KEY_CPAD_UP) { if (sel > 0) sel--; follow = true; }
            if (kd & KEY_RIGHT) tune_step(1);
            if (kd & KEY_LEFT) tune_step(-1);
            if (kd & KEY_A) {
                if (nview && view[sel] == cur && is_playing()) player_stop();
                else if (nview) tune_to(view[sel]);
            }
            if (kd & KEY_B) player_stop();
            if (kd & KEY_X) toggle_fav();
            if (kd & KEY_Y) set_band(vband + 1);
            if (kr & KEY_R) change_volume(0.05f);
            if (kr & KEY_L) change_volume(-0.05f);
            if (select_tap) { screens_off = true; set_screens(false); }
            handle_touch(kd, kh, ku);
        }

        /* list scrolling physics */
        if (!(kh & KEY_TOUCH)) {
            scroll += scroll_vel;
            scroll_vel *= 0.9f;
            if (fabsf(scroll_vel) < 0.05f) scroll_vel = 0;
            if (follow) {
                float area = LIST_Y1 - LIST_Y0;
                float sy = (float)sel * ROW_H;
                float want = scroll;
                if (sy < scroll) want = sy;
                if (sy + ROW_H > scroll + area) want = sy + ROW_H - area;
                scroll += (want - scroll) * 0.3f;
            }
            float ms = max_scroll();
            if (scroll < 0) scroll += (0 - scroll) * 0.35f;
            if (scroll > ms) scroll += (ms - scroll) * 0.35f;
        }

        /* Lid shut: the 3DS cuts the speakers, so keep playing (no sleep, screens
         * off) only when headphones are in. Without them, let it sleep normally
         * and pick the station back up when the lid opens. */
        shell_t += dt;
        if (shell_t > 0.4f) {
            shell_t = 0;
            bool hp = false;
            if (audio_ok && R_SUCCEEDED(DSP_GetHeadphoneStatus(&hp))) {
                if (hp != headphones) logf_("headphones %s", hp ? "in" : "out");
                headphones = hp;
            }
            u8 shell = 1;
            if (R_SUCCEEDED(PTMU_GetShellState(&shell))) {
                if (shell == 0 && !lid_off) {
                    lid_off = true;
                    set_screens(false);
                    logf_("lid shut (playing %d, headphones %d)", is_playing() ? 1 : 0, headphones ? 1 : 0);
                }
                else if (shell == 1 && lid_off) { lid_off = false; if (!screens_off) set_screens(true); }
            }
        }

        bool want_sleep = !(is_playing() && headphones);
        if (want_sleep != sleep_allowed) { aptSetSleepAllowed(want_sleep); sleep_allowed = want_sleep; }

        if (name_anim < 1) name_anim += dt * 4;
        if (toast_t > 0) toast_t -= dt;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, C_BLACK);
        C2D_SceneBegin(top);
        draw_top(dt);
        C2D_TargetClear(bot, C_BLACK);
        C2D_SceneBegin(bot);
        draw_bottom();
        C3D_FrameEnd(0);
    }

    logf_("exit");
    aptUnhook(&apt_cookie);
    settings_save();
    player_exit();
    set_screens(true);
    aptSetSleepAllowed(true);
    if (net_ok) socExit();
    ptmuExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
