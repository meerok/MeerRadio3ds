#pragma once
#include <stdbool.h>
#include <stddef.h>

#define MAX_STATIONS 1500
#define APP_DIR "sdmc:/3ds/MeerRadio"

typedef enum { BAND_FM = 0, BAND_AM = 1 } Band;

typedef struct {
    Band  band;
    float freq;          /* MHz for FM, kHz for AM */
    char  name[64];
    char  url[384];
    bool  fav;
    bool  scanned;
} Station;

typedef struct {
    float volume;
    char  country[4];
    char  last[384];
    bool  static_on;
} Settings;

extern Station  g_st[MAX_STATIONS];
extern int      g_nst;
extern Settings g_set;
extern bool     g_first_run;   /* no settings file yet */

void stations_init(void);
void settings_save(void);
int  station_find_url(const char *url);
void station_toggle_fav(int idx);
void fold_ascii(const char *in, char *out, size_t n);
bool parse_freq(const char *name, Band *band, float *freq);

typedef enum { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE, SCAN_FAILED } ScanState;
bool        scan_has_cache(void);
void        scan_clear(void);
void        scan_start(void);
ScanState   scan_state(void);
int         scan_found(void);
const char *scan_msg(void);
int         scan_collect(void);   /* merge finished scan into g_st, returns stations added */
