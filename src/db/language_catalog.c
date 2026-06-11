#include "language_catalog.h"
#include <string.h>

const LanguageCatalogEntry LANGUAGE_CATALOG[] = {
    {"en", "English"},
    {"es", "Spanish"},
    {"fr", "French"},
    {"de", "German"},
    {"it", "Italian"},
    {"pt", "Portuguese"},
    {"nl", "Dutch"},
    {"ru", "Russian"},
    {"zh", "Chinese"},
    {"ja", "Japanese"},
    {"ko", "Korean"},
    {"ar", "Arabic"},
    {"hi", "Hindi"},
    {"tr", "Turkish"},
    {"pl", "Polish"},
    {"sv", "Swedish"},
    {"da", "Danish"},
    {"no", "Norwegian"},
    {"fi", "Finnish"},
    {"el", "Greek"},
    {"cs", "Czech"},
    {"ro", "Romanian"},
    {"hu", "Hungarian"},
    {"uk", "Ukrainian"},
    {"vi", "Vietnamese"},
};

const size_t LANGUAGE_CATALOG_COUNT = sizeof(LANGUAGE_CATALOG) / sizeof(LANGUAGE_CATALOG[0]);

const char *language_catalog_name(const char *code) {
    for (size_t i = 0; i < LANGUAGE_CATALOG_COUNT; i++) {
        if (strcmp(LANGUAGE_CATALOG[i].code, code) == 0) return LANGUAGE_CATALOG[i].name;
    }
    return NULL;
}
