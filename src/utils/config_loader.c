#define _XOPEN_SOURCE 700

#include "config_loader.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int  verbose_level    = 3;
int  http_port        = 8080;
int  https_port       = 8443;
bool ssl_enabled      = false;
char ssl_cert[256]    = {0};
char ssl_key[256]     = {0};
char trusted_proxies[512] = {0};

int load_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Failed to open config file");
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '\n' || line[0] == '#') continue;

        char *delim = strchr(line, '=');
        if (!delim) continue;

        *delim = '\0';
        char *key   = line;
        char *value = delim + 1;
        value[strcspn(value, "\n")] = '\0';

        if (strcmp(key, "verbose_level") == 0) {
            verbose_level = atoi(value);
        } else if (strcmp(key, "http_port") == 0) {
            http_port = atoi(value);
        } else if (strcmp(key, "https_port") == 0) {
            https_port = atoi(value);
        } else if (strcmp(key, "ssl_enabled") == 0) {
            ssl_enabled = atoi(value) != 0;
        } else if (strcmp(key, "ssl_cert") == 0) {
            strncpy(ssl_cert, value, sizeof(ssl_cert) - 1);
            ssl_cert[sizeof(ssl_cert) - 1] = '\0';
        } else if (strcmp(key, "ssl_key") == 0) {
            strncpy(ssl_key, value, sizeof(ssl_key) - 1);
            ssl_key[sizeof(ssl_key) - 1] = '\0';
        } else if (strcmp(key, "trusted_proxies") == 0) {
            strncpy(trusted_proxies, value, sizeof(trusted_proxies) - 1);
            trusted_proxies[sizeof(trusted_proxies) - 1] = '\0';
        }
    }

    fclose(file);
    return 0;
}
