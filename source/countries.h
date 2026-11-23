#pragma once

typedef struct {
    const char *code;   /* ISO 3166-1 alpha-2, as radio-browser.info uses */
    const char *name;
} Country;

extern const Country COUNTRY_LIST[];
extern const int COUNTRY_COUNT;

int         country_index(const char *code);       /* -1 if not in the list */
const char *country_name(const char *code);        /* code itself if unknown */
const char *country_from_system(void);             /* from the 3DS settings, NULL if unknown */
