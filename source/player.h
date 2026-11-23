#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PS_STOPPED,
    PS_CONNECTING,
    PS_BUFFERING,
    PS_PLAYING,
    PS_ERROR
} PlayerState;

#define VIS_BANDS 16

bool        player_init(void);           /* false if the DSP could not start */
void        player_exit(void);
void        player_play(const char *url);
void        player_stop(void);
PlayerState player_state(void);
int         player_buffer_pct(void);
int         player_bitrate(void);
int         player_channels(void);
void        player_get_title(char *out, size_t n);
void        player_get_error(char *out, size_t n);
void        player_set_volume(float v);
void        player_set_static(bool on);
void        player_get_levels(float out[VIS_BANDS]);
