#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shop.h"
#include "version.h"
#include "log.h"
#include "../../vendor/cjson/cJSON.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/* Read the full contents of a file. Caller must free(). */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Route handlers                                                       */
/* ------------------------------------------------------------------ */

void shop_catalog(struct mg_connection *c, struct mg_http_message *hm) {
    struct mg_str *ua = mg_http_get_header(hm, "User-Agent");

    /* Block the broken build that crashes on the catalog */
    if (ua) {
        char ua_tmp[512] = {0};
        size_t tn = ua->len < sizeof(ua_tmp) - 1 ? ua->len : sizeof(ua_tmp) - 1;
        memcpy(ua_tmp, ua->buf, tn);
        if (strstr(ua_tmp, "2870186")) {
            mg_http_reply(c, 404, "", "");
            return;
        }
    }

    /* Try to load from disk */
    char *raw = read_file("responses/catalog/shop.json");
    if (!raw) {
        /* Return an empty but valid catalog */
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"expiration\":\"9999-12-31T23:59:59.999Z\","
                      "\"refreshIntervalHrs\":24,"
                      "\"dailyPurchaseHrs\":24,"
                      "\"storefronts\":[]}");
        return;
    }

    cJSON *catalog = cJSON_Parse(raw);
    free(raw);

    if (!catalog) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"expiration\":\"9999-12-31T23:59:59.999Z\","
                      "\"refreshIntervalHrs\":24,"
                      "\"dailyPurchaseHrs\":24,"
                      "\"storefronts\":[]}");
        return;
    }

    /* Version-based catalog adjustments (same logic as Lawin/Neonite) */
    CyberVersion ver = {0};
    if (ua) {
        char ua_str[512] = {0};
        size_t n = ua->len < sizeof(ua_str) - 1 ? ua->len : sizeof(ua_str) - 1;
        memcpy(ua_str, ua->buf, n);
        ua_str[n] = '\0';
        cyber_parse_version(ua_str, &ver);
    }

    if (ver.build >= 30.10f) {
        /* Replace "Normal" tile size with "Size_1_x_2" for newer builds */
        char *raw_str = cJSON_PrintUnformatted(catalog);
        cJSON_Delete(catalog);

        /* Simple string replacement */
        const char *from = "\"Normal\"";
        const char *to   = "\"Size_1_x_2\"";
        size_t from_len  = strlen(from);
        size_t to_len    = strlen(to);
        size_t raw_len   = strlen(raw_str);

        /* Count occurrences */
        int count = 0;
        const char *p = raw_str;
        while ((p = strstr(p, from))) { count++; p += from_len; }

        char *result = malloc(raw_len + count * (to_len - from_len) + 1);
        char *wp = result;
        p = raw_str;
        const char *match;
        while ((match = strstr(p, from))) {
            size_t before = (size_t)(match - p);
            memcpy(wp, p, before); wp += before;
            memcpy(wp, to, to_len); wp += to_len;
            p = match + from_len;
        }
        strcpy(wp, p);
        free(raw_str);

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", result);
        free(result);
        return;
    }

    char *body = cJSON_PrintUnformatted(catalog);
    cJSON_Delete(catalog);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void shop_keychain(struct mg_connection *c, struct mg_http_message *hm) {
    char *raw = read_file("responses/keychain.json");
    if (raw) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", raw);
        free(raw);
    } else {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "[]");
    }
}

void shop_bulk_offers(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{}");
}
