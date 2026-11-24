/*
 * Meer Radio - streaming engine
 *
 *  net thread (one per tune)  ->  ring buffer  ->  decoder thread  ->  NDSP
 *
 * The net thread uses libcurl (HTTP/HTTPS, redirects, Shoutcast/Icecast ICY
 * metadata, .pls/.m3u playlists). The decoder thread feeds libmpg123 and
 * queues PCM to the DSP. While a station is connecting or buffering the
 * decoder plays soft "tuning static" so switching feels like a real radio.
 */
#include <3ds.h>
#include <curl/curl.h>
#include <mpg123.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "player.h"

#define RING_SIZE   (512 * 1024)
#define PREBUF      (48 * 1024)
#define NBUF        6
#define FRAMES      2048
#define CH          0
#define USER_AGENT  "MeerRadio/1.0 (Nintendo 3DS)"

static u8 *ring;
static size_t r_head, r_tail, r_count;
static LightLock lock;

static volatile u32  g_gen = 1;
static volatile bool g_active = false;
static volatile bool g_net_done = false;
static volatile bool g_got_data = false;
static volatile PlayerState g_state = PS_STOPPED;
static volatile int  g_bitrate = 0, g_chans = 0, g_pct = 0;
static volatile float g_vol = 0.8f;
static volatile bool g_static = true;
static volatile bool g_quit = false;
static volatile bool g_suspended = false;   /* HOME Menu / sleep: DSP belongs to the system */
static char g_title[256];

/* Old connections stuck in DNS or a slow connect can pile up when you flick
 * through stations quickly; cap them and start the newest when one frees up. */
#define MAX_NET_THREADS 3
#define NET_STACK   (160 * 1024)
#define DEC_STACK   (128 * 1024)
static volatile int g_net_alive = 0;
static char g_pending_url[512];
static volatile bool g_pending = false;
static char g_err[128];

static Thread dec_thread;
static s32 main_prio = 0x30;
static ndspWaveBuf wbuf[NBUF];
static s16 *pcm;
static float levels[NBUF][VIS_BANDS];
static bool dsp_ok = false;

/* ------------------------------------------------------------------ ring */

static void ring_reset(void) { r_head = r_tail = r_count = 0; }

static size_t ring_read(u8 *dst, size_t n) {
    LightLock_Lock(&lock);
    size_t k = n < r_count ? n : r_count;
    size_t first = RING_SIZE - r_tail;
    if (first > k) first = k;
    memcpy(dst, ring + r_tail, first);
    if (k > first) memcpy(dst + first, ring, k - first);
    r_tail = (r_tail + k) % RING_SIZE;
    r_count -= k;
    LightLock_Unlock(&lock);
    return k;
}

/* returns bytes written, or -1 if this session is no longer current */
static int ring_write(const u8 *src, size_t n, u32 gen) {
    LightLock_Lock(&lock);
    if (gen != g_gen) { LightLock_Unlock(&lock); return -1; }
    size_t space = RING_SIZE - r_count;
    size_t k = n < space ? n : space;
    size_t first = RING_SIZE - r_head;
    if (first > k) first = k;
    memcpy(ring + r_head, src, first);
    if (k > first) memcpy(ring, src + first, k - first);
    r_head = (r_head + k) % RING_SIZE;
    r_count += k;
    if (k) g_got_data = true;
    LightLock_Unlock(&lock);
    return (int)k;
}

/* --------------------------------------------------------------- session */

enum { K_UNKNOWN = 0, K_AUDIO, K_PLAYLIST, K_UNSUPPORTED };

typedef struct {
    u32 gen;
    char url[512];
    int kind;
    char ctype[96];
    char unsup[64];
    int metaint;
    int mstate;            /* 0 audio, 1 meta length byte, 2 meta body */
    size_t audio_left;
    size_t meta_left, meta_pos;
    char meta[4096 + 1];
    char *pl;
    size_t pl_len;
} Session;

static void set_title_from_meta(const char *meta) {
    const char *p = strstr(meta, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *e = strstr(p, "';");
    if (!e) e = p + strlen(p);
    size_t n = (size_t)(e - p);
    if (n >= sizeof(g_title)) n = sizeof(g_title) - 1;
    LightLock_Lock(&lock);
    memcpy(g_title, p, n);
    g_title[n] = 0;
    LightLock_Unlock(&lock);
}

static bool push_audio(Session *s, const u8 *p, size_t n) {
    while (n) {
        int k = ring_write(p, n, s->gen);
        if (k < 0) return false;
        p += k;
        n -= (size_t)k;
        if (n) svcSleepThread(20 * 1000000LL);
    }
    return true;
}

static bool icy_process(Session *s, const u8 *d, size_t n) {
    size_t i = 0;
    if (s->metaint <= 0) return push_audio(s, d, n);
    while (i < n) {
        if (s->mstate == 0) {
            size_t k = n - i;
            if (k > s->audio_left) k = s->audio_left;
            if (k && !push_audio(s, d + i, k)) return false;
            i += k;
            s->audio_left -= k;
            if (s->audio_left == 0) s->mstate = 1;
        } else if (s->mstate == 1) {
            size_t len = (size_t)d[i++] * 16;
            if (len == 0) {
                s->audio_left = (size_t)s->metaint;
                s->mstate = 0;
            } else {
                s->meta_left = len;
                s->meta_pos = 0;
                s->mstate = 2;
            }
        } else {
            size_t k = n - i;
            if (k > s->meta_left) k = s->meta_left;
            if (s->meta_pos + k <= 4096) memcpy(s->meta + s->meta_pos, d + i, k);
            s->meta_pos += k;
            s->meta_left -= k;
            i += k;
            if (s->meta_left == 0) {
                s->meta[s->meta_pos < 4096 ? s->meta_pos : 4096] = 0;
                set_title_from_meta(s->meta);
                s->audio_left = (size_t)s->metaint;
                s->mstate = 0;
            }
        }
    }
    return true;
}

static bool ends_with_ci(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static void decide_kind(Session *s, const u8 *d, size_t n) {
    const char *ct = s->ctype;
    char path[512];
    snprintf(path, sizeof path, "%s", s->url);
    char *q = strchr(path, '?');
    if (q) *q = 0;

    if (strstr(ct, "mpegurl") || strstr(ct, "scpls") || strstr(ct, "x-pls")) { s->kind = K_PLAYLIST; return; }
    if (strstr(ct, "aac") || strstr(ct, "mp4") || strstr(ct, "ogg") || strstr(ct, "opus") || strstr(ct, "flac")) {
        s->kind = K_UNSUPPORTED;
        snprintf(s->unsup, sizeof s->unsup, "AAC/OGG stream - only MP3 is supported");
        return;
    }
    if (strstr(ct, "audio/mpeg") || strstr(ct, "audio/mp3") || strstr(ct, "audio/x-mpeg")) { s->kind = K_AUDIO; return; }

    if (ends_with_ci(path, ".pls") || ends_with_ci(path, ".m3u") || ends_with_ci(path, ".m3u8")) { s->kind = K_PLAYLIST; return; }
    if (n >= 10 && (!strncasecmp((const char *)d, "[playlist]", 10))) { s->kind = K_PLAYLIST; return; }
    if (n >= 7 && (!strncmp((const char *)d, "#EXTM3U", 7) || !strncasecmp((const char *)d, "http", 4))) { s->kind = K_PLAYLIST; return; }
    if (n >= 1 && d[0] == '<') {
        s->kind = K_UNSUPPORTED;
        snprintf(s->unsup, sizeof s->unsup, "Not an audio stream (web page)");
        return;
    }
    s->kind = K_AUDIO; /* unknown type: let mpg123 try */
}

static size_t hdr_cb(char *b, size_t sz, size_t nm, void *u) {
    Session *s = (Session *)u;
    size_t len = sz * nm;
    char line[256];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, b, n);
    line[n] = 0;
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;

    if (!strncasecmp(line, "HTTP/", 5) || !strncasecmp(line, "ICY ", 4)) {
        s->ctype[0] = 0;
        s->metaint = 0;
    } else if (!strncasecmp(line, "content-type:", 13)) {
        const char *v = line + 13;
        while (*v == ' ') v++;
        size_t i = 0;
        for (; v[i] && i < sizeof(s->ctype) - 1; i++) {
            char c = v[i];
            s->ctype[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        s->ctype[i] = 0;
    } else if (!strncasecmp(line, "icy-metaint:", 12)) {
        s->metaint = atoi(line + 12);
    } else if (!strncasecmp(line, "icy-br:", 7)) {
        int br = atoi(line + 7);
        if (br > 0 && br < 1000) g_bitrate = br;
    }
    return len;
}

static size_t wr_cb(char *d, size_t sz, size_t nm, void *u) {
    Session *s = (Session *)u;
    size_t len = sz * nm;
    if (s->gen != g_gen) return 0;
    if (s->kind == K_UNKNOWN) {
        decide_kind(s, (const u8 *)d, len);
        if (s->kind == K_AUDIO) { s->audio_left = (size_t)s->metaint; s->mstate = 0; }
    }
    if (s->kind == K_PLAYLIST) {
        if (s->pl_len + len > 64 * 1024) return len; /* enough playlist read */
        char *np = realloc(s->pl, s->pl_len + len + 1);
        if (!np) return 0;
        s->pl = np;
        memcpy(s->pl + s->pl_len, d, len);
        s->pl_len += len;
        s->pl[s->pl_len] = 0;
        return len;
    }
    if (s->kind == K_UNSUPPORTED) return 0;
    if (!icy_process(s, (const u8 *)d, len)) return 0;
    return len;
}

static int prog_cb(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    Session *s = (Session *)u;
    return s->gen != g_gen;
}

/* returns 1 found, 2 HLS, 0 nothing */
static int parse_playlist(const char *pl, char *out, size_t outn) {
    if (!pl) return 0;
    if (strstr(pl, "#EXT-X-")) return 2;
    const char *p = pl;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        char line[600];
        size_t n = (size_t)(e - p);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        char *v = line;
        while (*v == ' ' || *v == '\t') v++;
        if (!strncasecmp(v, "file", 4)) {
            char *eq = strchr(v, '=');
            if (eq) v = eq + 1;
        }
        if (!strncasecmp(v, "http://", 7) || !strncasecmp(v, "https://", 8)) {
            size_t L = strlen(v);
            while (L && (v[L - 1] == ' ' || v[L - 1] == '\t')) v[--L] = 0;
            snprintf(out, outn, "%s", v);
            return 1;
        }
        p = e;
        while (*p == '\n' || *p == '\r') p++;
    }
    return 0;
}

static void net_main(void *arg) {
    Session *s = (Session *)arg;
    char err[128] = "";
    int hops = 0;

    for (;;) {
        CURL *c = curl_easy_init();
        if (!c) { snprintf(err, sizeof err, "Network init failed"); break; }
        struct curl_slist *h = curl_slist_append(NULL, "Icy-MetaData: 1");
        s->kind = K_UNKNOWN;
        s->metaint = 0;
        s->mstate = 0;
        s->audio_left = 0;
        s->ctype[0] = 0;
        s->pl_len = 0;

        curl_easy_setopt(c, CURLOPT_URL, s->url);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 12L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 15L);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hdr_cb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, s);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, prog_cb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, s);

        CURLcode rc = curl_easy_perform(c);
        curl_slist_free_all(h);
        curl_easy_cleanup(c);

        if (s->gen != g_gen) goto out;

        if (s->kind == K_PLAYLIST) {
            char next[512];
            int r = parse_playlist(s->pl, next, sizeof next);
            if (r == 2) { snprintf(err, sizeof err, "HLS stream - not supported"); break; }
            if (r == 1 && hops < 4 && strcmp(next, s->url) != 0) {
                snprintf(s->url, sizeof s->url, "%s", next);
                hops++;
                continue;
            }
            snprintf(err, sizeof err, "Playlist has no playable stream");
            break;
        }
        if (s->kind == K_UNSUPPORTED) { snprintf(err, sizeof err, "%s", s->unsup); break; }
        if (rc != CURLE_OK) { snprintf(err, sizeof err, "%s", curl_easy_strerror(rc)); break; }
        snprintf(err, sizeof err, "Station stopped broadcasting");
        break;
    }

    LightLock_Lock(&lock);
    if (s->gen == g_gen) {
        g_net_done = true;
        snprintf(g_err, sizeof g_err, "%s", err);
    }
    LightLock_Unlock(&lock);
out:
    free(s->pl);
    free(s);
    LightLock_Lock(&lock);
    g_net_alive--;
    LightLock_Unlock(&lock);
}

/* ------------------------------------------------------------- decoding */

static void apply_mix(void) {
    float mix[12];
    memset(mix, 0, sizeof mix);
    mix[0] = mix[1] = g_vol;
    ndspChnSetMix(CH, mix);
}

static void configure(int rate, int chans) {
    ndspChnReset(CH);
    ndspChnSetInterp(CH, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CH, (float)rate);
    ndspChnSetFormat(CH, chans == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    apply_mix();
}

static void clear_bufs(void) {
    ndspChnWaveBufClear(CH);
    for (int i = 0; i < NBUF; i++) {
        wbuf[i].status = NDSP_WBUF_DONE;
        memset(levels[i], 0, sizeof levels[i]);
    }
}

static void queue_buf(int i, size_t frames, int chans) {
    wbuf[i].nsamples = (u32)frames;
    DSP_FlushDataCache(wbuf[i].data_vaddr, frames * (size_t)chans * 2);
    ndspChnWaveBufAdd(CH, &wbuf[i]);
}

static const float band_hz[VIS_BANDS] = {
    60, 100, 160, 250, 350, 500, 700, 1000, 1400, 2000, 2800, 4000, 5600, 7500, 10000, 13000
};

static void compute_levels(int idx, const s16 *s, size_t frames, int chans, int rate) {
    int N = frames < 512 ? (int)frames : 512;
    if (N < 64) return;
    for (int b = 0; b < VIS_BANDS; b++) {
        float f = band_hz[b];
        if (f >= rate * 0.5f) { levels[idx][b] = 0; continue; }
        float w = 2.0f * (float)M_PI * f / (float)rate;
        float coeff = 2.0f * cosf(w);
        float q1 = 0, q2 = 0;
        for (int i = 0; i < N; i++) {
            float x = chans == 2 ? ((float)s[2 * i] + (float)s[2 * i + 1]) * 0.5f : (float)s[i];
            x *= (1.0f / 32768.0f);
            float q0 = coeff * q1 - q2 + x;
            q2 = q1;
            q1 = q0;
        }
        float mag2 = q1 * q1 + q2 * q2 - coeff * q1 * q2;
        if (mag2 < 0) mag2 = 0;
        float mag = sqrtf(mag2) / (N * 0.5f);
        float db = 20.0f * log10f(mag + 1e-6f);
        float lv = (db + 54.0f) / 48.0f;
        if (lv < 0) lv = 0;
        if (lv > 1) lv = 1;
        levels[idx][b] = lv;
    }
}

static u32 noise_seed = 0x1234567u;
static float noise_lp = 0, noise_ph = 0;

static void gen_static(int idx, s16 *out, int frames, int chans) {
    for (int i = 0; i < frames; i++) {
        noise_seed = noise_seed * 1664525u + 1013904223u;
        float w = (float)((noise_seed >> 16) & 0xFFFF) / 32768.0f - 1.0f;
        noise_lp = noise_lp * 0.55f + w * 0.45f;
        float crack = 0;
        if ((noise_seed & 0x7FF) == 0) crack = (noise_seed & 0x10000) ? 0.8f : -0.8f;
        noise_ph += 0.00002f;
        if (noise_ph > 1) noise_ph -= 1;
        float amp = 0.07f + 0.04f * sinf(noise_ph * 6.2831853f * 3.0f);
        float v = noise_lp * amp + crack * 0.12f;
        s16 sv = (s16)(v * 32767.0f);
        out[i * chans] = sv;
        if (chans == 2) out[i * chans + 1] = sv;
    }
    for (int b = 0; b < VIS_BANDS; b++) {
        noise_seed = noise_seed * 1664525u + 1013904223u;
        levels[idx][b] = 0.05f + (float)((noise_seed >> 24) & 0x3F) / 400.0f;
    }
}

static void dec_main(void *arg) {
    (void)arg;
    int err = 0;
    mpg123_handle *mh = mpg123_new(NULL, &err);
    if (!mh) return;
    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);
    mpg123_format_none(mh);
    const long *rates;
    size_t nrates;
    mpg123_rates(&rates, &nrates);
    for (size_t i = 0; i < nrates; i++)
        mpg123_format(mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    mpg123_open_feed(mh);

    u32 my_gen = 0;
    int rate = 44100, chans = 2;
    bool fmt_known = false, prebuf = true;
    int errs = 0, cur = 0;
    static u8 inbuf[4096];

    configure(rate, chans);

    while (!g_quit) {
        if (g_suspended) { svcSleepThread(20 * 1000000LL); continue; }
        u32 gen = g_gen;
        if (gen != my_gen) {
            my_gen = gen;
            mpg123_close(mh);
            mpg123_open_feed(mh);
            clear_bufs();
            prebuf = true;
            fmt_known = false;
            errs = 0;
            cur = 0;
            if (!g_active) g_state = PS_STOPPED;
        }
        if (!g_active) { svcSleepThread(10 * 1000000LL); continue; }

        ndspWaveBuf *wb = &wbuf[cur];
        if (wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING) {
            svcSleepThread(2 * 1000000LL);
            continue;
        }
        s16 *out = (s16 *)wb->data_vaddr;

        if (prebuf) {
            size_t cnt;
            bool nd, got;
            LightLock_Lock(&lock);
            cnt = r_count;
            nd = g_net_done;
            got = g_got_data;
            LightLock_Unlock(&lock);
            if (nd && cnt == 0) {
                g_active = false;
                g_state = g_err[0] ? PS_ERROR : PS_STOPPED;
                clear_bufs();
                continue;
            }
            if (cnt >= PREBUF || (nd && cnt > 0)) {
                prebuf = false;
            } else {
                g_pct = (int)(cnt * 100 / PREBUF);
                g_state = got ? PS_BUFFERING : PS_CONNECTING;
                if (g_static) {
                    gen_static(cur, out, FRAMES, chans);
                    queue_buf(cur, FRAMES, chans);
                    cur = (cur + 1) % NBUF;
                } else {
                    svcSleepThread(10 * 1000000LL);
                }
                continue;
            }
        }

        size_t want = (size_t)FRAMES * chans * 2, got = 0;
        bool underrun = false;
        while (got < want && g_gen == my_gen && g_active) {
            size_t done = 0;
            int r = mpg123_read(mh, (unsigned char *)out + got, want - got, &done);
            got += done;
            if (r == MPG123_NEW_FORMAT) {
                long rt;
                int ch, enc;
                mpg123_getformat(mh, &rt, &ch, &enc);
                if (!fmt_known || rt != rate || ch != chans) {
                    rate = (int)rt;
                    chans = ch;
                    clear_bufs();
                    configure(rate, chans);
                    want = (size_t)FRAMES * chans * 2;
                    got = 0;
                }
                fmt_known = true;
                g_chans = chans;
                continue;
            }
            if (r == MPG123_NEED_MORE) {
                size_t k = ring_read(inbuf, sizeof inbuf);
                if (k == 0) { underrun = true; break; }
                mpg123_feed(mh, inbuf, k);
                continue;
            }
            if (r == MPG123_OK || r == MPG123_DONE) { errs = 0; continue; }
            if (++errs > 300) {
                snprintf(g_err, sizeof g_err, "Can't decode this stream");
                g_active = false;
                g_state = PS_ERROR;
                break;
            }
            size_t k = ring_read(inbuf, sizeof inbuf);
            if (k == 0) { underrun = true; break; }
            mpg123_feed(mh, inbuf, k);
        }
        if (g_gen != my_gen || !g_active) continue;

        /* after a clear_bufs() the current slot may have been reset; that's fine */
        size_t frames = got / ((size_t)chans * 2);
        if (frames > 0) {
            compute_levels(cur, out, frames, chans, rate);
            queue_buf(cur, frames, chans);
            cur = (cur + 1) % NBUF;
            g_state = PS_PLAYING;
        }
        if (underrun) prebuf = true;
    }
    mpg123_delete(mh);
}

/* ------------------------------------------------------------------ API */

bool player_init(void) {
    LightLock_Init(&lock);
    ring = (u8 *)malloc(RING_SIZE);
    curl_global_init(CURL_GLOBAL_ALL);
    mpg123_init();

    if (R_FAILED(ndspInit())) return false;
    dsp_ok = true;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    pcm = (s16 *)linearAlloc(NBUF * FRAMES * 2 * sizeof(s16));
    if (!pcm || !ring) return false;
    memset(wbuf, 0, sizeof wbuf);
    for (int i = 0; i < NBUF; i++) {
        wbuf[i].data_vaddr = pcm + i * FRAMES * 2;
        wbuf[i].status = NDSP_WBUF_DONE;
    }

    svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
    /* Below the main thread, so the app can always answer the system (HOME,
     * power button, sleep) even while audio is busy. Six buffers of headroom
     * keep playback smooth. */
    s32 dprio = main_prio + 1;
    if (dprio > 0x3F) dprio = 0x3F;
    /* Decoding stays on the app core. A thread on the New 3DS's extra core can be
     * paused by the system while it holds a lock when you press HOME, which froze
     * the whole console. MP3 decoding is light enough for the app core. */
    dec_thread = threadCreate(dec_main, NULL, DEC_STACK, dprio, -2, false);
    return dec_thread != NULL;
}

void player_exit(void) {
    player_stop();
    g_quit = true;
    if (dec_thread) {
        threadJoin(dec_thread, U64_MAX);
        threadFree(dec_thread);
    }
    if (dsp_ok) {
        ndspChnWaveBufClear(CH);
        ndspExit();
    }
    if (pcm) linearFree(pcm);
    /* net threads are detached; give them a moment to notice */
    svcSleepThread(200 * 1000000LL);
    curl_global_cleanup();
    mpg123_exit();
    free(ring);
}

static void start_net(const char *url) {
    Session *s = (Session *)calloc(1, sizeof(Session));
    if (!s) {
        LightLock_Lock(&lock);
        snprintf(g_err, sizeof g_err, "Out of memory");
        g_net_done = true;
        LightLock_Unlock(&lock);
        return;
    }
    LightLock_Lock(&lock);
    s->gen = g_gen;
    g_net_alive++;
    LightLock_Unlock(&lock);
    snprintf(s->url, sizeof s->url, "%s", url);

    s32 nprio = main_prio + 2;
    if (nprio > 0x3F) nprio = 0x3F;
    Thread t = threadCreate(net_main, s, NET_STACK, nprio, -2, true);
    if (!t) {
        LightLock_Lock(&lock);
        g_net_alive--;
        snprintf(g_err, sizeof g_err, "Couldn't start network thread");
        g_net_done = true;
        LightLock_Unlock(&lock);
        free(s);
    }
}

void player_play(const char *url) {
    if (!dsp_ok || !url || !url[0]) return;
    LightLock_Lock(&lock);
    g_gen++;
    ring_reset();
    g_net_done = false;
    g_got_data = false;
    g_err[0] = 0;
    g_title[0] = 0;
    g_bitrate = 0;
    g_chans = 0;
    g_pct = 0;
    g_state = PS_CONNECTING;
    g_active = true;
    bool busy = g_net_alive >= MAX_NET_THREADS;
    if (busy) {
        snprintf(g_pending_url, sizeof g_pending_url, "%s", url);
        g_pending = true;
    } else {
        g_pending = false;
    }
    LightLock_Unlock(&lock);
    if (!busy) start_net(url);
}

void player_tick(void) {
    if (!g_pending) return;
    char url[512];
    LightLock_Lock(&lock);
    bool go = g_pending && g_active && g_net_alive < MAX_NET_THREADS;
    if (go) {
        snprintf(url, sizeof url, "%s", g_pending_url);
        g_pending = false;
    }
    LightLock_Unlock(&lock);
    if (go) start_net(url);
}

void player_set_suspended(bool on) { g_suspended = on; }

void player_stop(void) {
    LightLock_Lock(&lock);
    g_gen++;
    g_active = false;
    g_pending = false;
    ring_reset();
    g_net_done = true;
    g_state = PS_STOPPED;
    g_title[0] = 0;
    LightLock_Unlock(&lock);
}

PlayerState player_state(void) { return g_state; }
int player_buffer_pct(void) { return g_pct; }
int player_bitrate(void) { return g_bitrate; }
int player_channels(void) { return g_chans; }

void player_get_title(char *out, size_t n) {
    LightLock_Lock(&lock);
    snprintf(out, n, "%s", g_title);
    LightLock_Unlock(&lock);
}

void player_get_error(char *out, size_t n) {
    LightLock_Lock(&lock);
    snprintf(out, n, "%s", g_err);
    LightLock_Unlock(&lock);
}

void player_set_volume(float v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    g_vol = v;
    if (dsp_ok) apply_mix();
}

void player_set_static(bool on) { g_static = on; }

void player_get_levels(float out[VIS_BANDS]) {
    for (int i = 0; i < NBUF; i++) {
        if (wbuf[i].status == NDSP_WBUF_PLAYING) {
            memcpy(out, levels[i], sizeof(float) * VIS_BANDS);
            return;
        }
    }
    memset(out, 0, sizeof(float) * VIS_BANDS);
}
