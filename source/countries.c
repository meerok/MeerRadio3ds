#include <3ds.h>
#include <string.h>
#include <strings.h>
#include "countries.h"

const Country COUNTRY_LIST[] = {
    {"AR", "Argentina"},      {"AU", "Australia"},      {"AT", "Austria"},        {"BE", "Belgium"},
    {"BO", "Bolivia"},        {"BR", "Brazil"},         {"BG", "Bulgaria"},       {"CA", "Canada"},
    {"CL", "Chile"},          {"CN", "China"},          {"CO", "Colombia"},       {"CR", "Costa Rica"},
    {"HR", "Croatia"},        {"CU", "Cuba"},           {"CZ", "Czechia"},        {"DK", "Denmark"},
    {"DO", "Dominican Rep."}, {"EC", "Ecuador"},        {"EG", "Egypt"},          {"SV", "El Salvador"},
    {"EE", "Estonia"},        {"FI", "Finland"},        {"FR", "France"},         {"DE", "Germany"},
    {"GH", "Ghana"},          {"GR", "Greece"},         {"GT", "Guatemala"},      {"HN", "Honduras"},
    {"HK", "Hong Kong"},      {"HU", "Hungary"},        {"IS", "Iceland"},        {"IN", "India"},
    {"ID", "Indonesia"},      {"IE", "Ireland"},        {"IL", "Israel"},         {"IT", "Italy"},
    {"JM", "Jamaica"},        {"JP", "Japan"},          {"KE", "Kenya"},          {"LV", "Latvia"},
    {"LT", "Lithuania"},      {"LU", "Luxembourg"},     {"MY", "Malaysia"},       {"MT", "Malta"},
    {"MX", "Mexico"},         {"MA", "Morocco"},        {"NL", "Netherlands"},    {"NZ", "New Zealand"},
    {"NI", "Nicaragua"},      {"NG", "Nigeria"},        {"NO", "Norway"},         {"PK", "Pakistan"},
    {"PA", "Panama"},         {"PY", "Paraguay"},       {"PE", "Peru"},           {"PH", "Philippines"},
    {"PL", "Poland"},         {"PT", "Portugal"},       {"PR", "Puerto Rico"},    {"RO", "Romania"},
    {"RU", "Russia"},         {"RS", "Serbia"},         {"SG", "Singapore"},      {"SK", "Slovakia"},
    {"SI", "Slovenia"},       {"ZA", "South Africa"},   {"KR", "South Korea"},    {"ES", "Spain"},
    {"SE", "Sweden"},         {"CH", "Switzerland"},    {"TW", "Taiwan"},         {"TH", "Thailand"},
    {"TR", "Turkey"},         {"UA", "Ukraine"},        {"AE", "UAE"},            {"GB", "United Kingdom"},
    {"US", "United States"},  {"UY", "Uruguay"},        {"VE", "Venezuela"},      {"VN", "Vietnam"},
};
const int COUNTRY_COUNT = (int)(sizeof(COUNTRY_LIST) / sizeof(COUNTRY_LIST[0]));

int country_index(const char *code) {
    for (int i = 0; i < COUNTRY_COUNT; i++)
        if (!strcasecmp(COUNTRY_LIST[i].code, code)) return i;
    return -1;
}

const char *country_name(const char *code) {
    int i = country_index(code);
    return i >= 0 ? COUNTRY_LIST[i].name : code;
}

/* Nintendo's country IDs (config block 0xB0000, byte 3) for the countries above */
static const struct { u8 id; const char *iso; } NINTENDO_IDS[] = {
    {1, "JP"},  {10, "AR"}, {15, "BO"}, {16, "BR"}, {18, "CA"}, {20, "CL"}, {21, "CO"}, {22, "CR"},
    {24, "DO"}, {25, "EC"}, {26, "SV"}, {30, "GT"}, {33, "HN"}, {34, "JM"}, {36, "MX"}, {39, "NI"},
    {40, "PA"}, {41, "PY"}, {42, "PE"}, {49, "US"}, {50, "UY"}, {52, "VE"}, {65, "AU"}, {66, "AT"},
    {67, "BE"}, {70, "BG"}, {71, "HR"}, {73, "CZ"}, {74, "DK"}, {75, "EE"}, {76, "FI"}, {77, "FR"},
    {78, "DE"}, {79, "GR"}, {80, "HU"}, {81, "IS"}, {82, "IE"}, {83, "IT"}, {84, "LV"}, {87, "LT"},
    {88, "LU"}, {90, "MT"}, {94, "NL"}, {95, "NZ"}, {96, "NO"}, {97, "PL"}, {98, "PT"}, {99, "RO"},
    {100, "RU"}, {101, "RS"}, {102, "SK"}, {103, "SI"}, {104, "ZA"}, {105, "ES"}, {107, "SE"},
    {108, "CH"}, {109, "TR"}, {110, "GB"}, {128, "TW"}, {136, "KR"}, {144, "HK"}, {152, "ID"},
    {153, "SG"}, {154, "TH"}, {155, "PH"}, {156, "MY"}, {160, "CN"}, {168, "AE"}, {169, "IN"},
    {170, "EG"},
};

const char *country_from_system(void) {
    if (R_FAILED(cfguInit())) return NULL;
    u8 info[4] = {0};
    Result rc = CFGU_GetConfigInfoBlk2(4, 0x000B0000, info);
    cfguExit();
    if (R_FAILED(rc)) return NULL;
    for (size_t i = 0; i < sizeof(NINTENDO_IDS) / sizeof(NINTENDO_IDS[0]); i++)
        if (NINTENDO_IDS[i].id == info[3]) return NINTENDO_IDS[i].iso;
    return NULL;
}
