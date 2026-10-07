#ifndef COUNTRY_CONTINENT_H
#define COUNTRY_CONTINENT_H

// Maps a country name, exactly as analytics.c's GeoIP lookup stores it in
// page_visits_daily.by_country (GeoLite2's English "country.names.en"), to
// its GeoLite2 continent name ("Europe", "South America", ...). Done at
// report time from a static table rather than recorded per visit, so it
// also works for every day bucket written before continents existed.
// Returns NULL for "Unknown" or any name not in the table.
const char *country_continent(const char *country);

#endif // COUNTRY_CONTINENT_H
