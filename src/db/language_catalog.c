#include "language_catalog.h"
#include <string.h>

const LanguageCatalogEntry LANGUAGE_CATALOG[] = {
    {"en", "English", "English", "Eng"},
    {"es", "Spanish", "Español", "Esp"},
    {"fr", "French", "Français", "Fra"},
    {"de", "German", "Deutsch", "Deu"},
    {"it", "Italian", "Italiano", "Ita"},
    {"pt", "Portuguese", "Português", "Por"},
    {"nl", "Dutch", "Nederlands", "Ned"},
    {"ru", "Russian", "Русский", "Рус"},
    {"zh", "Chinese", "中文", "中文"},
    {"ja", "Japanese", "日本語", "日本"},
    {"ko", "Korean", "한국어", "한국"},
    {"ar", "Arabic", "العربية", "عربي"},
    {"hi", "Hindi", "हिन्दी", "हिन"},
    {"tr", "Turkish", "Türkçe", "Tür"},
    {"pl", "Polish", "Polski", "Pol"},
    {"sv", "Swedish", "Svenska", "Sve"},
    {"da", "Danish", "Dansk", "Dan"},
    {"no", "Norwegian", "Norsk", "Nor"},
    {"fi", "Finnish", "Suomi", "Suo"},
    {"el", "Greek", "Ελληνικά", "Ελλ"},
    {"cs", "Czech", "Čeština", "Češ"},
    {"ro", "Romanian", "Română", "Rom"},
    {"hu", "Hungarian", "Magyar", "Mag"},
    {"uk", "Ukrainian", "Українська", "Укр"},
    {"vi", "Vietnamese", "Tiếng Việt", "Việt"},
};

const size_t LANGUAGE_CATALOG_COUNT = sizeof(LANGUAGE_CATALOG) / sizeof(LANGUAGE_CATALOG[0]);

const char *language_catalog_name(const char *code) {
    for (size_t i = 0; i < LANGUAGE_CATALOG_COUNT; i++) {
        if (strcmp(LANGUAGE_CATALOG[i].code, code) == 0) return LANGUAGE_CATALOG[i].name;
    }
    return NULL;
}

const char *language_catalog_native(const char *code) {
    if (!code) return "";
    for (size_t i = 0; i < LANGUAGE_CATALOG_COUNT; i++)
        if (strcmp(LANGUAGE_CATALOG[i].code, code) == 0)
            return LANGUAGE_CATALOG[i].native;
    return code;
}

const char *language_catalog_abbr(const char *code) {
    if (!code) return "";
    for (size_t i = 0; i < LANGUAGE_CATALOG_COUNT; i++)
        if (strcmp(LANGUAGE_CATALOG[i].code, code) == 0)
            return LANGUAGE_CATALOG[i].abbr;
    return code;
}
