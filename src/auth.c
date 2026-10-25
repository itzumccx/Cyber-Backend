#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "auth.h"
#include "errors.h"
#include "log.h"
#include "../../vendor/mongoose/mongoose.h"
#include "../../vendor/cjson/cJSON.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static int form_get(const char *body, size_t body_len,
                    const char *key, char *out, size_t out_len) {
    struct mg_str b = mg_str_n(body, body_len);
    if (!mg_http_get_var(&b, key, out, out_len)) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

static void now_iso(char *buf, size_t len) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S.000Z", tm);
}

/* Parse major version number from Fortnite User-Agent */
static int parse_version_global(struct mg_http_message *hm) {
    struct mg_str *ua = mg_http_get_header(hm, "User-Agent");
    if (!ua || !ua->buf) return 1;

    const char *p = NULL;
    for (size_t i = 0; i + 8 <= ua->len; i++) {
        if (strncmp(ua->buf + i, "Release-", 8) == 0) {
            p = ua->buf + i + 8;
            break;
        }
    }
    if (!p) return 1;

    int major = 0;
    while (*p >= '0' && *p <= '9') {
        major = major * 10 + (*p - '0');
        p++;
    }
    return major > 0 ? major : 1;
}

/* Extract account_id from our token: eg1~id.name.cyber */
static void token_get_account_id(const char *token, char *out, size_t out_len) {
    const char *t = token;
    if (strncmp(t, "bearer ", 7) == 0) t += 7;
    if (strncmp(t, "Bearer ", 7) == 0) t += 7;
    if (strncmp(t, "eg1~",    4) == 0) t += 4;

    const char *dot = strchr(t, '.');
    size_t n = dot ? (size_t)(dot - t) : strlen(t);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, t, n);
    out[n] = '\0';
}

/* Extract display_name from our token */
static void token_get_display_name(const char *token, char *out, size_t out_len) {
    const char *t = token;
    if (strncmp(t, "bearer ", 7) == 0) t += 7;
    if (strncmp(t, "Bearer ", 7) == 0) t += 7;
    if (strncmp(t, "eg1~",    4) == 0) t += 4;

    const char *d1 = strchr(t, '.');
    if (!d1) { strncpy(out, t, out_len - 1); out[out_len-1] = '\0'; return; }
    d1++;
    const char *d2 = strchr(d1, '.');
    size_t n = d2 ? (size_t)(d2 - d1) : strlen(d1);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, d1, n);
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* JWT payload decoder — extracts "sub" and "dn" from a real Epic JWT  */
/* A JWT is header.payload.sig — the payload is base64url-encoded JSON */
/* ------------------------------------------------------------------ */

static int base64url_decode(const char *src, size_t src_len, char *out, size_t out_len) {
    /* Add padding if needed */
    char padded[4096] = {0};
    if (src_len >= sizeof(padded) - 4) return 0;
    memcpy(padded, src, src_len);
    size_t pad = (4 - src_len % 4) % 4;
    for (size_t i = 0; i < pad; i++) padded[src_len + i] = '=';
    size_t total = src_len + pad;

    /* Replace URL-safe chars */
    for (size_t i = 0; i < total; i++) {
        if (padded[i] == '-') padded[i] = '+';
        else if (padded[i] == '_') padded[i] = '/';
    }

    /* Simple base64 decode */
    static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_pos = 0;
    for (size_t i = 0; i + 3 < total && out_pos + 2 < out_len; i += 4) {
        unsigned int v = 0;
        for (int j = 0; j < 4; j++) {
            char ch = padded[i + j];
            if (ch == '=') break;
            const char *p = strchr(b64, ch);
            if (!p) return 0;
            v = (v << 6) | (unsigned int)(p - b64);
        }
        if (out_pos < out_len - 1) out[out_pos++] = (char)((v >> 16) & 0xFF);
        if (padded[i+2] != '=' && out_pos < out_len - 1) out[out_pos++] = (char)((v >> 8) & 0xFF);
        if (padded[i+3] != '=' && out_pos < out_len - 1) out[out_pos++] = (char)((v >> 0) & 0xFF);
    }
    out[out_pos] = '\0';
    return (int)out_pos;
}

/* Try to decode a JWT and extract "sub" → account_id, "dn" → display_name.
   Returns 1 on success, 0 if the token is not a valid JWT. */
static int jwt_decode_fields(const char *token,
                             char *account_id, size_t aid_len,
                             char *display_name, size_t dn_len) {
    const char *t = token;
    if (strncmp(t, "bearer ", 7) == 0) t += 7;
    if (strncmp(t, "Bearer ", 7) == 0) t += 7;
    /* Skip "eg1~" prefix if present */
    if (strncmp(t, "eg1~", 4) == 0) t += 4;

    /* A JWT has exactly two dots: header.payload.sig */
    const char *dot1 = strchr(t, '.');
    if (!dot1) return 0;
    const char *payload_b64 = dot1 + 1;
    const char *dot2 = strchr(payload_b64, '.');
    if (!dot2) return 0;
    size_t payload_len = (size_t)(dot2 - payload_b64);

    char payload_json[2048] = {0};
    if (!base64url_decode(payload_b64, payload_len, payload_json, sizeof(payload_json)))
        return 0;

    cJSON *obj = cJSON_Parse(payload_json);
    if (!obj) return 0;

    int found = 0;

    /* "sub" → account_id */
    cJSON *sub = cJSON_GetObjectItem(obj, "sub");
    if (cJSON_IsString(sub) && sub->valuestring && sub->valuestring[0]) {
        strncpy(account_id, sub->valuestring, aid_len - 1);
        account_id[aid_len - 1] = '\0';
        found = 1;
    }

    /* "dn" → display_name (Epic JWTs use "dn" for display name) */
    cJSON *dn = cJSON_GetObjectItem(obj, "dn");
    if (cJSON_IsString(dn) && dn->valuestring && dn->valuestring[0]) {
        strncpy(display_name, dn->valuestring, dn_len - 1);
        display_name[dn_len - 1] = '\0';
        found = 1;
    }

    cJSON_Delete(obj);
    return found;
}

/* ------------------------------------------------------------------ */
/* Real Epic client_credentials fetch (same as Neonite for v>=28)     */
/* Mongoose async inside a temporary mini event-loop.                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char token[1024];
    int  done;
    int  ok;
} cc_state_t;

static void cc_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    cc_state_t *s = (cc_state_t *)c->fn_data;

    if (ev == MG_EV_CONNECT) {
        const char *body = "grant_type=client_credentials&token_type=eg1";
        int blen = (int)strlen(body);
        mg_printf(c,
            "POST /account/api/oauth/token HTTP/1.1\r\n"
            "Host: account-public-service-prod.ol.epicgames.com\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Authorization: Basic ZWM2ODRiOGM2ODdmNDc5ZmFkZWEzY2IyYWQ4M2Y1YzY6ZTFmMzFjMjExZjI4NDEzMTg2MjYyZDM3YTEzZmM4NGQ=\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s", blen, body);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        cJSON *root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
        if (root) {
            cJSON *tok = cJSON_GetObjectItem(root, "access_token");
            if (cJSON_IsString(tok) && tok->valuestring[0]) {
                strncpy(s->token, tok->valuestring, sizeof(s->token) - 1);
                s->ok = 1;
            }
            cJSON_Delete(root);
        }
        s->done = 1;
        c->is_closing = 1;
    } else if (ev == MG_EV_ERROR || ev == MG_EV_CLOSE) {
        s->done = 1;
    }
}

/* Returns 1 and fills out_token on success */
static int fetch_epic_token(char *out_token, size_t out_len) {
    cc_state_t state = {0};
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    struct mg_connection *conn = mg_http_connect(&mgr,
        "https://account-public-service-prod.ol.epicgames.com",
        cc_ev_handler, &state);

    if (!conn) {
        mg_mgr_free(&mgr);
        return 0;
    }

    /* Poll up to 5 seconds */
    for (int i = 0; i < 50 && !state.done; i++) {
        mg_mgr_poll(&mgr, 100);
    }
    mg_mgr_free(&mgr);

    if (state.ok && state.token[0]) {
        strncpy(out_token, state.token, out_len - 1);
        out_token[out_len - 1] = '\0';
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Build and send the OAuth response                                    */
/* ------------------------------------------------------------------ */

static void send_token_response(struct mg_connection *c,
                                const char *account_id,
                                const char *display_name,
                                int version_global) {
    char access_token[1024] = {0};
    char refresh_token[1024] = {0};

    /*
     * Like Neonite: for version >= 28 (Ch4+) the game validates the token
     * against Epic's servers, so we fetch a real client_credentials token.
     * The refresh_token stays as our own eg1~ format so /verify still works.
     */
    if (version_global >= 28) {
        if (fetch_epic_token(access_token, sizeof(access_token))) {
            cyber_log("Got real Epic token for %s (v%d)", display_name, version_global);
        } else {
            cyber_warn("Epic token fetch failed, using fallback for %s", display_name);
            snprintf(access_token, sizeof(access_token),
                     "eg1~%s.%s.cyber", account_id, display_name);
        }
    } else {
        snprintf(access_token, sizeof(access_token),
                 "eg1~%s.%s.cyber", account_id, display_name);
    }

    /* refresh_token is always our own format so we can decode it in /verify */
    snprintf(refresh_token, sizeof(refresh_token),
             "eg1~%s.%s.cyber", account_id, display_name);

    char now[64];
    now_iso(now, sizeof(now));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "access_token",       access_token);
    cJSON_AddNumberToObject(root, "expires_in",         2147483647);
    cJSON_AddStringToObject(root, "expires_at",         "9999-12-31T23:59:59.999Z");
    cJSON_AddStringToObject(root, "token_type",         "bearer");
    cJSON_AddStringToObject(root, "refresh_token",      refresh_token);
    cJSON_AddNumberToObject(root, "refresh_expires",    2147483647);
    cJSON_AddStringToObject(root, "refresh_expires_at", "9999-12-31T23:59:59.999Z");
    cJSON_AddStringToObject(root, "account_id",         account_id);
    cJSON_AddStringToObject(root, "client_id",          "ec684b8c687f479fadea3cb2ad83f5c6");
    cJSON_AddBoolToObject  (root, "internal_client",    1);
    cJSON_AddStringToObject(root, "client_service",     "prod-fn");

    cJSON *scope = cJSON_CreateArray();
    cJSON_AddItemToArray(scope, cJSON_CreateString("basic_profile"));
    cJSON_AddItemToArray(scope, cJSON_CreateString("friends_list"));
    cJSON_AddItemToArray(scope, cJSON_CreateString("openid"));
    cJSON_AddItemToArray(scope, cJSON_CreateString("presence"));
    cJSON_AddItemToObject(root, "scope", scope);

    cJSON_AddStringToObject(root, "displayName",    display_name);
    cJSON_AddStringToObject(root, "app",            "prod-fn");
    cJSON_AddStringToObject(root, "in_app_id",      account_id);
    cJSON_AddStringToObject(root, "product_id",     "prod-fn");
    cJSON_AddStringToObject(root, "application_id", "fghi4567FNFBKFz3E4TROb0bmPS8h1GW");
    cJSON_AddStringToObject(root, "acr",            "urn:epic:loa:aal1");
    cJSON_AddStringToObject(root, "auth_time",      now);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

/* ------------------------------------------------------------------ */
/* Route handlers                                                       */
/* ------------------------------------------------------------------ */

void auth_oauth_token(struct mg_connection *c, struct mg_http_message *hm) {
    char grant_type[64]    = {0};
    char username[128]     = {0};
    char display_name[128] = {0};
    char account_id[128]   = {0};

    form_get(hm->body.buf, hm->body.len, "grant_type", grant_type, sizeof(grant_type));

    int vg = parse_version_global(hm);

    if (strcmp(grant_type, "password") == 0) {
        form_get(hm->body.buf, hm->body.len, "username", username, sizeof(username));
        if (!username[0]) {
            cyber_send_error(c, 400, ERR_OAUTH_INVALID, "username is required.", 1013);
            return;
        }
        char *at = strchr(username, '@');
        if (at) {
            size_t n = at - username;
            memcpy(display_name, username, n);
            display_name[n] = '\0';
        } else {
            strncpy(display_name, username, sizeof(display_name) - 1);
        }
        strncpy(account_id, display_name, sizeof(account_id) - 1);
        for (char *p = account_id; *p; p++) if (*p == ' ') *p = '_';

    } else if (strcmp(grant_type, "authorization_code") == 0) {
        char code[128] = {0};
        form_get(hm->body.buf, hm->body.len, "code", code, sizeof(code));
        if (!code[0]) {
            cyber_send_error(c, 400, ERR_OAUTH_INVALID, "code is required.", 1013);
            return;
        }
        strncpy(display_name, code, sizeof(display_name) - 1);
        strncpy(account_id,   code, sizeof(account_id)   - 1);

    } else if (strcmp(grant_type, "device_auth") == 0) {
        form_get(hm->body.buf, hm->body.len, "account_id", account_id, sizeof(account_id));
        if (!account_id[0]) {
            cyber_send_error(c, 400, ERR_OAUTH_INVALID, "account_id is required.", 1013);
            return;
        }
        strncpy(display_name, account_id, sizeof(display_name) - 1);

    } else if (strcmp(grant_type, "exchange_code") == 0) {
        char code[128] = {0};
        form_get(hm->body.buf, hm->body.len, "exchange_code", code, sizeof(code));
        if (!code[0]) {
            cyber_send_error(c, 400, ERR_OAUTH_INVALID, "exchange_code is required.", 1013);
            return;
        }
        strncpy(display_name, code, sizeof(display_name) - 1);
        strncpy(account_id,   code, sizeof(account_id)   - 1);

    } else if (strcmp(grant_type, "client_credentials") == 0 ||
               strcmp(grant_type, "refresh_token")      == 0) {
        strncpy(display_name, "Cyber", sizeof(display_name) - 1);
        strncpy(account_id,   "Cyber", sizeof(account_id)   - 1);

    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Unsupported grant type: %s", grant_type);
        cyber_send_error(c, 400, ERR_OAUTH_UNSUPPORTED, msg, 1016);
        return;
    }

    cyber_log("Login: %s (%s) v%d", display_name, grant_type, vg);
    send_token_response(c, account_id, display_name, vg);
}

void auth_verify_token(struct mg_connection *c, struct mg_http_message *hm) {
    char auth[512] = {0};
    struct mg_str *h = mg_http_get_header(hm, "Authorization");
    if (h) {
        size_t n = h->len < sizeof(auth) - 1 ? h->len : sizeof(auth) - 1;
        memcpy(auth, h->buf, n);
        auth[n] = '\0';
    }

    char account_id[128]   = {0};
    char display_name[128] = {0};

    /*
     * For Ch6/Ch7 (version >= 28) the game sends back the REAL Epic JWT
     * as the access_token. We must decode it via JWT, not via the custom
     * eg1~id.name.cyber format. Try JWT decode first; fall back to legacy.
     */
    if (!jwt_decode_fields(auth, account_id, sizeof(account_id),
                                 display_name, sizeof(display_name))) {
        /* Not a JWT — legacy custom token format: eg1~id.name.cyber */
        token_get_account_id(auth, account_id, sizeof(account_id));
        token_get_display_name(auth, display_name, sizeof(display_name));
    }

    if (!account_id[0])    strncpy(account_id,    "Cyber", sizeof(account_id)    - 1);
    if (!display_name[0])  strncpy(display_name,  account_id, sizeof(display_name) - 1);

    char now[64];
    now_iso(now, sizeof(now));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "token",           auth);
    cJSON_AddStringToObject(root, "token_type",      "bearer");
    cJSON_AddStringToObject(root, "client_id",       "ec684b8c687f479fadea3cb2ad83f5c6");
    cJSON_AddBoolToObject  (root, "internal_client", 1);
    cJSON_AddStringToObject(root, "client_service",  "prod-fn");
    cJSON_AddStringToObject(root, "account_id",      account_id);
    cJSON_AddNumberToObject(root, "expires_in",      2147483647);
    cJSON_AddStringToObject(root, "expires_at",      "9999-12-31T23:59:59.999Z");
    cJSON_AddStringToObject(root, "auth_method",     "authorization_code");
    cJSON_AddStringToObject(root, "display_name",    display_name);
    cJSON_AddStringToObject(root, "app",             "prod-fn");
    cJSON_AddStringToObject(root, "in_app_id",       account_id);
    cJSON_AddStringToObject(root, "product_id",      "prod-fn");
    cJSON_AddStringToObject(root, "sandbox_id",      "fn");
    cJSON_AddStringToObject(root, "deployment_id",   "62a9473a2dca46b29ccf17577fcf42d7");
    cJSON_AddStringToObject(root, "application_id",  "fghi4567FNFBKFz3E4TROb0bmPS8h1GW");
    cJSON_AddStringToObject(root, "acr",             "urn:epic:loa:aal1");
    cJSON_AddStringToObject(root, "auth_time",       now);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void auth_account_info(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    const char *last = strrchr(hm->uri.buf, '/');
    if (last) {
        last++;
        size_t n = hm->uri.len - (last - hm->uri.buf);
        if (n >= sizeof(account_id)) n = sizeof(account_id) - 1;
        memcpy(account_id, last, n);
        account_id[n] = '\0';
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id",                         account_id);
    cJSON_AddStringToObject(root, "displayName",                account_id);
    cJSON_AddStringToObject(root, "email",                      "cyber@dev.com");
    cJSON_AddNumberToObject(root, "failedLoginAttempts",        0);
    cJSON_AddStringToObject(root, "lastLogin",                  "");
    cJSON_AddNumberToObject(root, "numberOfDisplayNameChanges", 1);
    cJSON_AddStringToObject(root, "dateOfBirth",                "1999-01-01");
    cJSON_AddStringToObject(root, "ageGroup",                   "ADULT");
    cJSON_AddBoolToObject  (root, "headless",                   0);
    cJSON_AddStringToObject(root, "country",                    "CA");
    cJSON_AddStringToObject(root, "preferredLanguage",          "en");
    cJSON_AddBoolToObject  (root, "canUpdateDisplayName",       1);
    cJSON_AddBoolToObject  (root, "tfaEnabled",                 1);
    cJSON_AddBoolToObject  (root, "emailVerified",              1);
    cJSON_AddBoolToObject  (root, "minorVerified",              0);
    cJSON_AddStringToObject(root, "minorStatus",                "NOT_MINOR");
    cJSON_AddBoolToObject  (root, "cabinedMode",                0);
    cJSON_AddItemToObject  (root, "externalAuths",              cJSON_CreateObject());

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void auth_display_name(struct mg_connection *c, struct mg_http_message *hm) {
    char display_name[128] = {0};
    const char *last = strrchr(hm->uri.buf, '/');
    if (last) {
        last++;
        size_t n = hm->uri.len - (last - hm->uri.buf);
        if (n >= sizeof(display_name)) n = sizeof(display_name) - 1;
        memcpy(display_name, last, n);
        display_name[n] = '\0';
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id",           display_name);
    cJSON_AddStringToObject(root, "displayName",  display_name);
    cJSON_AddItemToObject  (root, "externalAuths",cJSON_CreateObject());

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void auth_public_account(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    mg_http_get_var(&hm->query, "accountId", account_id, sizeof(account_id));

    cJSON *arr  = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id",           account_id);
    cJSON_AddStringToObject(item, "displayName",  account_id);
    cJSON_AddItemToObject  (item, "externalAuths",cJSON_CreateObject());
    cJSON_AddItemToArray(arr, item);

    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void auth_external_auths(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "[]");
}

void auth_discovery_token(struct mg_connection *c, struct mg_http_message *hm) {
    char branch[64] = "++Fortnite+Release-36.20";
    struct mg_str *ua = mg_http_get_header(hm, "User-Agent");
    if (ua) {
        const char *buf = ua->buf;
        size_t len = ua->len;
        for (size_t i = 0; i + 8 <= len; i++) {
            if (strncmp(buf + i, "Release-", 8) == 0 && i >= 2) {
                const char *p = buf + i - 2;
                size_t j = 0;
                while (*p && *p != ' ' && j < sizeof(branch) - 1)
                    branch[j++] = *p++;
                branch[j] = '\0';
                break;
            }
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "branchName", branch);
    cJSON_AddStringToObject(root, "appId",      "Fortnite");
    cJSON_AddStringToObject(root, "token",      "cyberdiscovery123=");

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void auth_token_info(struct mg_connection *c, struct mg_http_message *hm) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject  (root, "active",     1);
    cJSON_AddStringToObject(root, "token_type", "bearer");
    cJSON_AddNumberToObject(root, "expires_in", 2147483647);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void auth_turn_credentials(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{}");
}
