#pragma once
#include <3ds.h>
#include <stdbool.h>

/* citro2d colour order is ABGR in a u32: r | g<<8 | b<<16 | a<<24 */
#define RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))
#define RGB(r, g, b) RGBA(r, g, b, 255)

/* Sweetie-16 pixel palette */
#define C_BLACK   RGB(26, 28, 44)
#define C_PURPLE  RGB(93, 39, 93)
#define C_RED     RGB(177, 62, 83)
#define C_ORANGE  RGB(239, 125, 87)
#define C_YELLOW  RGB(255, 205, 117)
#define C_LGREEN  RGB(167, 240, 112)
#define C_GREEN   RGB(56, 183, 100)
#define C_TEAL    RGB(37, 113, 121)
#define C_NAVY    RGB(41, 54, 111)
#define C_BLUE    RGB(59, 93, 201)
#define C_SKY     RGB(65, 166, 246)
#define C_CYAN    RGB(115, 239, 247)
#define C_WHITE   RGB(244, 244, 244)
#define C_LGRAY   RGB(148, 176, 194)
#define C_GRAY    RGB(86, 108, 134)
#define C_DGRAY   RGB(51, 60, 87)

#define LCD_BG    RGB(14, 18, 24)
#define LCD_GHOST RGB(30, 30, 32)
#define LCD_DIM   RGB(120, 92, 54)

typedef enum {
    IC_PLAY, IC_STOP, IC_PREV, IC_HEART, IC_SCAN, IC_SPEAKER, IC_NOTE, IC_COUNT
} Icon;

void ui_init(void);
void ui_rect(float x, float y, float w, float h, u32 c);
void ui_frame(float x, float y, float w, float h, int t, u32 c);
void ui_bevel(float x, float y, float w, float h, u32 face, u32 hi, u32 lo);
int  ui_text_w(const char *s, int scale);
void ui_text(float x, float y, const char *s, int scale, u32 c);
void ui_text_clip(float x, float y, const char *s, int scale, u32 c, float cx0, float cx1);
void ui_text_shadow(float x, float y, const char *s, int scale, u32 c, u32 sh);
void ui_text_center(float cx, float y, const char *s, int scale, u32 c);
void ui_icon(Icon ic, float x, float y, int scale, u32 c, bool mirror);
u32  ui_alpha(u32 c, float a);
u32  ui_mix(u32 a, u32 b, float t);
