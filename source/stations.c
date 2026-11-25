/*
 * Meer Radio - station list, settings and the online "SCAN"
 *
 * stations.txt  : your own list (created with defaults on first run)
 * scanned.txt   : stations found by SCAN (radio-browser.info, MP3 only,
 *                 kept only if the name contains a real AM/FM frequency)
 * settings.txt  : volume, country, last station, favourites
 *
 * Line format for station files:   FM|90.3|KEXP Seattle|https://...
 */
#include <3ds.h>
#include <curl/curl.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "stations.h"

#define STATIONS_FILE APP_DIR "/stations.txt"
#define SCANNED_FILE  APP_DIR "/scanned.txt"
#define SETTINGS_FILE APP_DIR "/settings.txt"
#define MAX_FAVS 256

Station  g_st[MAX_STATIONS];
int      g_nst = 0;
Settings g_set;
bool     g_first_run = false;

static char favs[MAX_FAVS][384];
static int nfavs = 0;

static const char *DEFAULT_HEADER =
    "# Meer Radio station list\n"
    "# Format: BAND|FREQUENCY|NAME|STREAM URL|COUNTRY   (MP3 streams only)\n"
    "# BAND is FM (MHz) or AM (kHz). COUNTRY is a 2-letter code like US or GB:\n"
    "# the station only shows when that country is picked. Leave it off to show it everywhere.\n"
    "# Lines starting with # are ignored. Tip: SCAN pulls in real AM/FM stations for your country.\n";

/* Presets, each tied to its own country so they don't show up everywhere */
static const char *DEFAULT_STATIONS[] = {
    "FM|88.5|KNKX Seattle-Tacoma|http://live.wostreaming.net/direct/ppm-knkxfmmp3-ibc1|US",
    "FM|90.3|KEXP Seattle|https://kexp-mp3-128.streamguys1.com/kexp128.mp3|US",
    "FM|91.1|WFMU Jersey City|http://stream0.wfmu.org/freeform-128k|US",
    "FM|93.9|WNYC New York|https://fm939.wnyc.org/wnycfm|US",
    "FM|94.9|KUOW Seattle|http://playerservices.streamtheworld.com/pls/KUOWFM_HIGH_MP3.pls|US",
    "FM|98.1|Classical KING FM|http://classicalking.streamguys1.com/king-fm-mp3|US",
    "FM|105.9|WQXR New York|https://stream.wqxr.org/wqxr|US",
    "FM|100.9|Classic FM|http://ice-sov.musicradio.com/ClassicFMMP3|GB",
    "FM|97.7|97.7 HTZ-FM St. Catharines|https://playerservices.streamtheworld.com/api/livestream-redirect/CHTZFM_SC|CA",
    "FM|92.7|Majic 92.7 WDCJ|https://playerservices.streamtheworld.com/api/livestream-redirect/WDCJFM_SC|US",
    "FM|93.9|WKYS Washington DC|https://playerservices.streamtheworld.com/api/livestream-redirect/WKYSFM_SC|US",
    "FM|95.5|WPGC Washington DC|https://live.amperwave.net/direct/audacy-wpgcfmmp3-imc|US",
    "FM|102.3|Majic 102.3 WMMJ|https://playerservices.streamtheworld.com/api/livestream-redirect/WMMJFM_SC|US",
    "FM|103.5|WTOP News|https://playerservices.streamtheworld.com/api/livestream-redirect/WTOPFM_SC|US",
    "FM|105.1|WAVA Washington DC|https://playerservices.streamtheworld.com/api/livestream-redirect/WAVAFM_SC|US",
    "FM|105.7|WJZ The Fan Baltimore|https://live.amperwave.net/direct/audacy-wjzfmmp3-imc|US",
    "FM|105.9|WMAL Washington DC|https://playerservices.streamtheworld.com/api/livestream-redirect/WMALFM_SC|US",
    "FM|106.7|WJFK The Fan|https://live.amperwave.net/direct/audacy-wjfkfmmp3-imc|US",
    "FM|107.5|BB Radio|https://streambbr.ir-media-tec.com/live/mp3-128/dmhubweb/play.mp3|DE",
    "FM|104.8|Ostseewelle|https://ostseewelle.stream30.radiohost.de/ostseewelle-nord_mp3-192|DE",
};
#define N_DEFAULTS (int)(sizeof(DEFAULT_STATIONS) / sizeof(DEFAULT_STATIONS[0]))

/* ------------------------------------------------------------ helpers */

static const char FOLD_C3[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuypy";

void fold_ascii(const char *in, char *out, size_t n) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 1 < n) {
        unsigned char c = *p;
        if (c < 0x80) {
            out[o++] = (c < 32) ? ' ' : (char)c;
            p++;
        } else if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
            out[o++] = FOLD_C3[p[1] - 0x80];
            p += 2;
        } else if (c == 0xE2 && p[1] == 0x80 && p[2]) {
            unsigned char d = p[2];
            if (d == 0x93 || d == 0x94) out[o++] = '-';
            else if (d == 0x98 || d == 0x99) out[o++] = '\'';
            else if (d == 0x9C || d == 0x9D) out[o++] = '"';
            else if (d == 0xA2) out[o++] = '*';
            p += 3;
        } else {
            p++;
            while ((*p & 0xC0) == 0x80) p++;
        }
    }
    out[o] = 0;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static bool ci2(const char *p, char a, char b) {
    return toupper((unsigned char)p[0]) == a && toupper((unsigned char)p[1]) == b;
}

/* Finds a broadcast frequency inside a station name, e.g.
 * "CFRA 580", "CBC Radio One 91.5", "CJRT-FM 91.1", "Z103.5", "News 1010 AM". */
bool parse_freq(const char *name, Band *band, float *freq) {
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)name[i])) continue;
        if (i > 0 && (isdigit((unsigned char)name[i - 1]) || name[i - 1] == '.' || name[i - 1] == ',')) continue;
        bool prevAlpha = i > 0 && isalpha((unsigned char)name[i - 1]);

        size_t j = i;
        while (j < n && isdigit((unsigned char)name[j])) j++;
        bool dec = false;
        size_t k = j;
        if (j + 1 < n && (name[j] == '.' || name[j] == ',') && isdigit((unsigned char)name[j + 1])) {
            dec = true;
            k = j + 1;
            while (k < n && isdigit((unsigned char)name[k])) k++;
        }
        char buf[16];
        size_t L = k - i;
        if (L >= sizeof buf) { i = k; continue; }
        memcpy(buf, name + i, L);
        buf[L] = 0;
        for (size_t q = 0; q < L; q++) if (buf[q] == ',') buf[q] = '.';
        float v = (float)atof(buf);

        /* words after the number */
        size_t a = k;
        while (a < n && (name[a] == ' ' || name[a] == '-')) a++;
        bool fmA = a + 1 < n && ci2(name + a, 'F', 'M') && (a + 2 >= n || !isalpha((unsigned char)name[a + 2]));
        bool amA = a + 1 < n && ci2(name + a, 'A', 'M') && (a + 2 >= n || !isalpha((unsigned char)name[a + 2]));
        bool mhz = a + 2 < n && !strncasecmp(name + a, "MHZ", 3);
        bool khz = a + 2 < n && !strncasecmp(name + a, "KHZ", 3);
        /* words before the number */
        size_t b = i;
        while (b > 0 && (name[b - 1] == ' ' || name[b - 1] == '-')) b--;
        bool fmB = b >= 2 && ci2(name + b - 2, 'F', 'M') && (b < 3 || !isalpha((unsigned char)name[b - 3]));
        bool amB = b >= 2 && ci2(name + b - 2, 'A', 'M') && (b < 3 || !isalpha((unsigned char)name[b - 3]));

        bool glued = k < n && isalpha((unsigned char)name[k]) && !(fmA || amA || mhz || khz);
        if (glued) { i = k; continue; }
        /* "Top 1000 Hits", "1000 Classics": counts, not frequencies */
        if (!dec && !(amA || amB || khz) &&
            ((b >= 3 && !strncasecmp(name + b - 3, "top", 3)) || !strncasecmp(name + a, "hit", 3) ||
             !strncasecmp(name + a, "song", 4) || !strncasecmp(name + a, "classic", 7))) { i = k; continue; }

        if ((fmA || fmB || mhz) && v >= 87.5f && v <= 108.05f) {
            *band = BAND_FM; *freq = roundf(v * 10.0f) / 10.0f; return true;
        }
        if ((amA || amB || khz) && !dec && v >= 530 && v <= 1710) {
            *band = BAND_AM; *freq = v; return true;
        }
        if (dec && v >= 87.5f && v <= 108.05f) {
            *band = BAND_FM; *freq = roundf(v * 10.0f) / 10.0f; return true;
        }
        if (!dec && !prevAlpha && !(fmA || fmB) && v >= 530 && v <= 1710) {
            *band = BAND_AM; *freq = v; return true;
        }
        i = k;
    }
    return false;
}

static bool parse_line(char *line, Station *st) {
    trim(line);
    if (!line[0] || line[0] == '#') return false;
    char *f[5];
    int nf = 0;
    char *p = line;
    f[nf++] = p;
    while (*p && nf < 5) {
        if (*p == '|') { *p = 0; f[nf++] = p + 1; }
        p++;
    }
    if (nf < 4) return false;
    for (int i = 0; i < nf; i++) trim(f[i]);
    memset(st, 0, sizeof *st);
    if (!strcasecmp(f[0], "AM")) st->band = BAND_AM;
    else if (!strcasecmp(f[0], "FM")) st->band = BAND_FM;
    else return false;
    st->freq = (float)atof(f[1]);
    fold_ascii(f[2], st->name, sizeof st->name);
    snprintf(st->url, sizeof st->url, "%s", f[3]);
    if (nf >= 5) for (int i = 0; i < 2 && f[4][i]; i++) st->country[i] = (char)toupper((unsigned char)f[4][i]);
    return st->url[0] && st->freq > 0;
}

int station_find_url(const char *url) {
    if (!url || !url[0]) return -1;
    for (int i = 0; i < g_nst; i++)
        if (!strcmp(g_st[i].url, url)) return i;
    return -1;
}

static bool is_dup(const Station *list, int n, const Station *s) {
    for (int i = 0; i < n; i++) {
        if (!strcmp(list[i].url, s->url)) return true;
        if (list[i].band == s->band && fabsf(list[i].freq - s->freq) < 0.05f &&
            !strncasecmp(list[i].name, s->name, 10))
            return true;
    }
    return false;
}

static int load_file(const char *path, bool scanned) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[600];
    int added = 0;
    while (fgets(line, sizeof line, f) && g_nst < MAX_STATIONS) {
        Station st;
        if (!parse_line(line, &st)) continue;
        st.scanned = scanned;
        if (is_dup(g_st, g_nst, &st)) continue;
        g_st[g_nst++] = st;
        added++;
    }
    fclose(f);
    return added;
}

static void apply_favs(void) {
    for (int i = 0; i < g_nst; i++) {
        g_st[i].fav = false;
        for (int j = 0; j < nfavs; j++)
            if (!strcmp(favs[j], g_st[i].url)) { g_st[i].fav = true; break; }
    }
}

void station_toggle_fav(int idx) {
    if (idx < 0 || idx >= g_nst) return;
    Station *s = &g_st[idx];
    s->fav = !s->fav;
    if (s->fav) {
        if (nfavs < MAX_FAVS) snprintf(favs[nfavs++], sizeof favs[0], "%s", s->url);
    } else {
        for (int j = 0; j < nfavs; j++) {
            if (!strcmp(favs[j], s->url)) {
                memmove(favs[j], favs[j + 1], (size_t)(nfavs - j - 1) * sizeof favs[0]);
                nfavs--;
                break;
            }
        }
    }
    settings_save();
}

static void settings_load(void) {
    g_set.volume = 0.8f;
    snprintf(g_set.country, sizeof g_set.country, "CA");
    g_set.last[0] = 0;
    g_set.static_on = true;
    nfavs = 0;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) { g_first_run = true; return; }
    char line[600];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "volume")) g_set.volume = (float)atof(v);
        else if (!strcmp(k, "country")) snprintf(g_set.country, sizeof g_set.country, "%.2s", v);
        else if (!strcmp(k, "last")) snprintf(g_set.last, sizeof g_set.last, "%s", v);
        else if (!strcmp(k, "static")) g_set.static_on = atoi(v) != 0;
        else if (!strcmp(k, "fav") && nfavs < MAX_FAVS) snprintf(favs[nfavs++], sizeof favs[0], "%s", v);
    }
    fclose(f);
    if (g_set.volume < 0 || g_set.volume > 1) g_set.volume = 0.8f;
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "volume=%.2f\ncountry=%s\nstatic=%d\nlast=%s\n", g_set.volume, g_set.country,
            g_set.static_on ? 1 : 0, g_set.last);
    for (int j = 0; j < nfavs; j++) fprintf(f, "fav=%s\n", favs[j]);
    fclose(f);
}

void stations_init(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(APP_DIR, 0777);
    settings_load();
    g_nst = 0;
    bool fresh = load_file(STATIONS_FILE, false) < 0;
    if (fresh) {
        FILE *f = fopen(STATIONS_FILE, "w");
        if (f) { fputs(DEFAULT_HEADER, f); fclose(f); }
    }
    /* Presets: tag old copies (written before presets had a country) with their
     * country, and add any preset this file doesn't have yet. */
    FILE *app = NULL;
    for (int d = 0; d < N_DEFAULTS; d++) {
        char line[600];
        Station def;
        snprintf(line, sizeof line, "%s", DEFAULT_STATIONS[d]);
        if (!parse_line(line, &def)) continue;
        int have = station_find_url(def.url);
        if (have >= 0) {
            if (!g_st[have].country[0]) snprintf(g_st[have].country, sizeof g_st[have].country, "%s", def.country);
            continue;
        }
        if (g_nst >= MAX_STATIONS) break;
        g_st[g_nst++] = def;
        if (!app) app = fopen(STATIONS_FILE, "a");
        if (app) fprintf(app, "%s\n", DEFAULT_STATIONS[d]);
    }
    if (app) fclose(app);
    load_file(SCANNED_FILE, true);
    apply_favs();
}

/* ----------------------------------------------------------------- scan */

typedef struct { char *d; size_t n, cap; } Buf;

static volatile ScanState s_state = SCAN_IDLE;
static volatile int s_found = 0;
static char s_msg[96];
static Station *s_res = NULL;
static int s_nres = 0;
static char s_country[4];

static size_t buf_cb(char *p, size_t sz, size_t nm, void *u) {
    Buf *b = (Buf *)u;
    size_t L = sz * nm;
    if (b->n + L + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256 * 1024;
        while (nc < b->n + L + 1) nc *= 2;
        if (nc > 6 * 1024 * 1024) return 0;
        char *nd = (char *)realloc(b->d, nc);
        if (!nd) return 0;
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->n, p, L);
    b->n += L;
    b->d[b->n] = 0;
    return L;
}

static void utf8_put(char *out, size_t *o, size_t n, unsigned cp) {
    if (cp < 0x80) { if (*o + 1 < n) out[(*o)++] = (char)cp; }
    else if (cp < 0x800) {
        if (*o + 2 < n) { out[(*o)++] = (char)(0xC0 | (cp >> 6)); out[(*o)++] = (char)(0x80 | (cp & 0x3F)); }
    } else {
        if (*o + 3 < n) {
            out[(*o)++] = (char)(0xE0 | (cp >> 12));
            out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*o)++] = (char)(0x80 | (cp & 0x3F));
        }
    }
}

/* reads a JSON string body (p points just after the opening quote) */
static const char *json_read(const char *p, char *out, size_t n) {
    size_t o = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p++;
            switch (c) {
                case 'n': case 't': case 'r': case 'b': case 'f': if (o + 1 < n) out[o++] = ' '; break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4 && isxdigit((unsigned char)*p); i++, p++) {
                        char h = *p;
                        cp = cp * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
                    }
                    utf8_put(out, &o, n, cp);
                    break;
                }
                default: if (o + 1 < n) out[o++] = c; break;
            }
        } else {
            if (o + 1 < n) out[o++] = *p;
            p++;
        }
    }
    out[o] = 0;
    return *p == '"' ? p + 1 : p;
}

static void scan_main(void *arg) {
    (void)arg;
    static const char *hosts[] = {
        "all.api.radio-browser.info", "de1.api.radio-browser.info",
        "nl1.api.radio-browser.info", "at1.api.radio-browser.info"
    };
    Buf b = {0};
    CURLcode rc = CURLE_FAILED_INIT;
    long code = 0;

    for (int h = 0; h < 4; h++) {
        char url[300];
        snprintf(url, sizeof url,
                 "https://%s/json/stations/search?countrycode=%s&codec=MP3&hidebroken=true"
                 "&order=clickcount&reverse=true&limit=1000",
                 hosts[h], s_country);
        snprintf(s_msg, sizeof s_msg, "Contacting radio directory...");
        b.n = 0;
        CURL *c = curl_easy_init();
        if (!c) break;
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "MeerRadio/1.0 (Nintendo 3DS)");
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 90L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
        rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
        if (rc == CURLE_OK && code == 200 && b.n > 2) break;
    }

    if (rc != CURLE_OK || code != 200 || !b.d) {
        snprintf(s_msg, sizeof s_msg, "Scan failed: %s", rc != CURLE_OK ? curl_easy_strerror(rc) : "server error");
        free(b.d);
        s_state = SCAN_FAILED;
        return;
    }

    snprintf(s_msg, sizeof s_msg, "Reading stations...");
    s_nres = 0;
    const char *p = b.d;
    while ((p = strstr(p, "\"name\":\"")) && s_nres < MAX_STATIONS) {
        char raw[256], url[512];
        p = json_read(p + 8, raw, sizeof raw);
        const char *nx = strstr(p, "\"name\":\"");
        const char *u = strstr(p, "\"url_resolved\":\"");
        if (!u || (nx && u > nx)) continue;
        json_read(u + 16, url, sizeof url);
        /* tags sometimes carry the frequency when the name doesn't ("news,95.3 fm") */
        char tags[256] = "";
        const char *tg = strstr(u, "\"tags\":\"");
        if (tg && (!nx || tg < nx)) json_read(tg + 8, tags, sizeof tags);
        if (strncasecmp(url, "http", 4)) continue;
        size_t ul = strlen(url);
        if (ul >= 384 || (ul > 5 && !strcasecmp(url + ul - 5, ".m3u8"))) continue;

        Station st;
        memset(&st, 0, sizeof st);
        fold_ascii(raw, st.name, sizeof st.name);
        trim(st.name);
        for (char *q = st.name; *q; q++) if (*q == '|') *q = '/';
        if (!st.name[0]) continue;
        if (!parse_freq(st.name, &st.band, &st.freq)) {
            char ft[256];
            fold_ascii(tags, ft, sizeof ft);
            if (!ft[0] || !parse_freq(ft, &st.band, &st.freq)) continue;
        }
        snprintf(st.url, sizeof st.url, "%s", url);
        st.scanned = true;
        if (is_dup(s_res, s_nres, &st)) continue;
        s_res[s_nres++] = st;
        s_found = s_nres;
    }
    free(b.d);

    FILE *f = fopen(SCANNED_FILE, "w");
    if (f) {
        fprintf(f, "# Found by SCAN (%s). This file is replaced on every scan.\n", s_country);
        for (int i = 0; i < s_nres; i++) {
            if (s_res[i].band == BAND_FM)
                fprintf(f, "FM|%.1f|%s|%s\n", s_res[i].freq, s_res[i].name, s_res[i].url);
            else
                fprintf(f, "AM|%d|%s|%s\n", (int)s_res[i].freq, s_res[i].name, s_res[i].url);
        }
        fclose(f);
    }
    snprintf(s_msg, sizeof s_msg, "Found %d stations", s_nres);
    s_state = SCAN_DONE;
}

/* Drops every scanned station (used when the country changes) */
void scan_clear(void) {
    int w = 0;
    for (int i = 0; i < g_nst; i++)
        if (!g_st[i].scanned) g_st[w++] = g_st[i];
    g_nst = w;
    remove(SCANNED_FILE);
}

bool scan_has_cache(void) {
    FILE *f = fopen(SCANNED_FILE, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

void scan_start(void) {
    if (s_state == SCAN_RUNNING) return;
    if (!s_res) s_res = (Station *)malloc(sizeof(Station) * MAX_STATIONS);
    if (!s_res) { snprintf(s_msg, sizeof s_msg, "Out of memory"); s_state = SCAN_FAILED; return; }
    snprintf(s_country, sizeof s_country, "%s", g_set.country);
    s_found = 0;
    s_nres = 0;
    s_state = SCAN_RUNNING;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = threadCreate(scan_main, NULL, 256 * 1024, prio + 2 > 0x3F ? 0x3F : prio + 2, -2, true);
    if (!t) { snprintf(s_msg, sizeof s_msg, "Couldn't start scan"); s_state = SCAN_FAILED; }
}

ScanState scan_state(void) { return s_state; }
int scan_found(void) { return s_found; }
const char *scan_msg(void) { return s_msg; }

int scan_collect(void) {
    int added = 0;
    if (s_state == SCAN_DONE) {
        int w = 0;
        for (int i = 0; i < g_nst; i++)
            if (!g_st[i].scanned) g_st[w++] = g_st[i];
        g_nst = w;
        for (int i = 0; i < s_nres && g_nst < MAX_STATIONS; i++) {
            if (is_dup(g_st, g_nst, &s_res[i])) continue;
            g_st[g_nst++] = s_res[i];
            added++;
        }
        apply_favs();
    }
    s_state = SCAN_IDLE;
    return added;
}

/* Presets with a country only show when that country is picked */
bool station_shows(int idx) {
    const Station *st = &g_st[idx];
    return st->scanned || !st->country[0] || !strcasecmp(st->country, g_set.country);
}
