#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "eos.h"
#include "log.h"
#include "../../vendor/cjson/cJSON.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static void now_iso(char *buf, size_t len) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S.000Z", tm);
}

/* Extract accountId/displayName from our token format eg1~id.name.cyber */
static void token_account_id(const char *token, char *out, size_t out_len) {
    const char *t = token;
    if (strncmp(t, "bearer ", 7) == 0) t += 7;
    if (strncmp(t, "Bearer ", 7) == 0) t += 7;
    if (strncmp(t, "eg1~", 4) == 0)   t += 4;
    const char *dot = strchr(t, '.');
    size_t n = dot ? (size_t)(dot - t) : strlen(t);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, t, n);
    out[n] = '\0';
}

static void token_display_name(const char *token, char *out, size_t out_len) {
    const char *t = token;
    if (strncmp(t, "bearer ", 7) == 0) t += 7;
    if (strncmp(t, "Bearer ", 7) == 0) t += 7;
    if (strncmp(t, "eg1~", 4) == 0)   t += 4;
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
/* JWT payload decoder — needed for Ch6/Ch7 real Epic JWT tokens        */
/* ------------------------------------------------------------------ */

static int eos_base64url_decode(const char *src, size_t src_len, char *out, size_t out_len) {
    char padded[4096] = {0};
    if (src_len >= sizeof(padded) - 4) return 0;
    memcpy(padded, src, src_len);
    size_t pad = (4 - src_len % 4) % 4;
    for (size_t i = 0; i < pad; i++) padded[src_len + i] = '=';
    size_t total = src_len + pad;
    for (size_t i = 0; i < total; i++) {
        if (padded[i] == '-') padded[i] = '+';
        else if (padded[i] == '_') padded[i] = '/';
    }
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

/* Decode a real Epic JWT token → extract "sub" (account_id) and "dn" (display_name).
   Returns 1 on success, 0 if token is not a valid JWT. */
static int eos_jwt_decode(const char *token,
                          char *account_id, size_t aid_len,
                          char *display_name, size_t dn_len) {
    const char *t = token;
    if (strncmp(t, "bearer ", 7) == 0) t += 7;
    if (strncmp(t, "Bearer ", 7) == 0) t += 7;
    if (strncmp(t, "eg1~", 4) == 0)   t += 4;

    const char *dot1 = strchr(t, '.');
    if (!dot1) return 0;
    const char *payload_b64 = dot1 + 1;
    const char *dot2 = strchr(payload_b64, '.');
    if (!dot2) return 0;
    size_t payload_len = (size_t)(dot2 - payload_b64);

    char payload_json[2048] = {0};
    if (!eos_base64url_decode(payload_b64, payload_len, payload_json, sizeof(payload_json)))
        return 0;

    cJSON *obj = cJSON_Parse(payload_json);
    if (!obj) return 0;

    int found = 0;
    cJSON *sub = cJSON_GetObjectItem(obj, "sub");
    if (cJSON_IsString(sub) && sub->valuestring && sub->valuestring[0]) {
        strncpy(account_id, sub->valuestring, aid_len - 1);
        account_id[aid_len - 1] = '\0';
        found = 1;
    }
    cJSON *dn = cJSON_GetObjectItem(obj, "dn");
    if (cJSON_IsString(dn) && dn->valuestring && dn->valuestring[0]) {
        strncpy(display_name, dn->valuestring, dn_len - 1);
        display_name[dn_len - 1] = '\0';
        found = 1;
    }
    cJSON_Delete(obj);
    return found;
}

/* Helper: extract account_id + display_name from any token format (JWT or eg1~) */
static void extract_identity(const char *token,
                              char *account_id, size_t aid_len,
                              char *display_name, size_t dn_len) {
    if (!eos_jwt_decode(token, account_id, aid_len, display_name, dn_len)) {
        token_account_id(token, account_id, aid_len);
        token_display_name(token, display_name, dn_len);
    }
    if (!account_id[0]) strncpy(account_id, "Cyber", aid_len - 1);
    if (!display_name[0]) strncpy(display_name, account_id, dn_len - 1);
}

/* Build an EOS feature list array */
static cJSON *eos_features(void) {
    cJSON *arr = cJSON_CreateArray();
    const char *features[] = {
        "AntiCheat","Connect","ContentService","Ecom","EpicConnect",
        "Inventories","LockerService","Matchmaking Service",
        "ExchangeCodeCreation","Achievements","Leaderboards",
        "Matchmaking","Metrics","PlayerReports","Sanctions","Stats",
        "TitleStorage","Voice","CommerceService","FNResonanceService",
        "MagpieService","PCBService","QuestService", NULL
    };
    for (int i = 0; features[i]; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(features[i]));
    return arr;
}

/* Build an EOS token string (same simple format as auth tokens) */
static void build_eos_token(const char *account_id, const char *display_name,
                             char *out, size_t out_len) {
    snprintf(out, out_len, "eg1~%s.%s.eos", account_id, display_name);
}

/* ------------------------------------------------------------------ */
/* Route handlers                                                       */
/* ------------------------------------------------------------------ */

void eos_oauth_v1(struct mg_connection *c, struct mg_http_message *hm) {
    char grant_type[64]    = {0};
    char deployment_id[64] = {0};
    char nonce[128]        = {0};
    char ext_token[512]    = {0};
    char refresh_tok[512]  = {0};

    mg_http_get_var(&hm->body, "grant_type",          grant_type,    sizeof(grant_type));
    mg_http_get_var(&hm->body, "deployment_id",       deployment_id, sizeof(deployment_id));
    mg_http_get_var(&hm->body, "nonce",               nonce,         sizeof(nonce));
    mg_http_get_var(&hm->body, "external_auth_token", ext_token,     sizeof(ext_token));
    mg_http_get_var(&hm->body, "refresh_token",       refresh_tok,   sizeof(refresh_tok));

    if (strcmp(grant_type, "client_credentials") == 0) {
        char token[256];
        build_eos_token("CyberEOS", "CyberEOS", token, sizeof(token));

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "access_token",    token);
        cJSON_AddStringToObject(root, "token_type",      "bearer");
        cJSON_AddStringToObject(root, "expires_at",      "9999-12-31T23:59:59.999Z");
        cJSON_AddItemToObject  (root, "features",        eos_features());
        cJSON_AddStringToObject(root, "organization_id", "o-aa83a0a9bc45e98c80c1b1c9d92e9e");
        cJSON_AddStringToObject(root, "product_id",      "prod-fn");
        cJSON_AddStringToObject(root, "sandbox_id",      "fn");
        cJSON_AddStringToObject(root, "deployment_id",   deployment_id);
        cJSON_AddNumberToObject(root, "expires_in",      3599);

        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
        free(body);
        return;
    }

    if (strcmp(grant_type, "external_auth") == 0) {
        char account_id[128]   = {0};
        char display_name[128] = {0};
        /* For Ch6/Ch7 the ext_token is a real Epic JWT — use extract_identity()
           which tries JWT decode first, then falls back to eg1~ format. */
        extract_identity(ext_token, account_id, sizeof(account_id),
                                    display_name, sizeof(display_name));

        char token[256], id_token[256];
        build_eos_token(account_id, display_name, token, sizeof(token));
        snprintf(id_token, sizeof(id_token), "eyeos~%s.%s", account_id, display_name);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "access_token",          token);
        cJSON_AddStringToObject(root, "token_type",            "bearer");
        cJSON_AddStringToObject(root, "expires_at",            "9999-12-31T23:59:59.999Z");
        cJSON_AddStringToObject(root, "nonce",                 nonce);
        cJSON_AddItemToObject  (root, "features",              eos_features());
        cJSON_AddStringToObject(root, "organization_id",       "o-aa83a0a9bc45e98c80c1b1c9d92e9e");
        cJSON_AddStringToObject(root, "product_id",            "prod-fn");
        cJSON_AddStringToObject(root, "sandbox_id",            "fn");
        cJSON_AddStringToObject(root, "deployment_id",         deployment_id);
        cJSON_AddStringToObject(root, "organization_user_id",  "000185f80b9a4dc3aaf1ca83611c2bf5");
        cJSON_AddStringToObject(root, "product_user_id",       "00027b91959a4c57a1272efcc4d7480f");
        cJSON_AddBoolToObject  (root, "product_user_id_created",0);
        cJSON_AddStringToObject(root, "id_token",              id_token);
        cJSON_AddNumberToObject(root, "expires_in",            3599);

        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
        free(body);
        return;
    }

    if (strcmp(grant_type, "refresh_token") == 0) {
        char account_id[128]   = {0};
        char display_name[128] = {0};
        /* refresh_tok may be a real Epic JWT (Ch6/Ch7) or our eg1~ format */
        extract_identity(refresh_tok, account_id, sizeof(account_id),
                                      display_name, sizeof(display_name));

        char token[256];
        build_eos_token(account_id, display_name, token, sizeof(token));

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "access_token",    token);
        cJSON_AddNumberToObject(root, "expires_in",      15552000);
        cJSON_AddStringToObject(root, "expires_at",      "9999-12-31T23:59:59.999Z");
        cJSON_AddStringToObject(root, "token_type",      "bearer");
        cJSON_AddStringToObject(root, "refresh_token",   token);
        cJSON_AddNumberToObject(root, "refresh_expires", 15552000);
        cJSON_AddStringToObject(root, "refresh_expires_at","9999-12-31T23:59:59.999Z");
        cJSON_AddStringToObject(root, "account_id",      account_id);
        cJSON_AddStringToObject(root, "displayName",     display_name);

        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
        free(body);
        return;
    }

    mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                  "{\"error\":\"unsupported_grant_type\"}");
}

void eos_oauth_v2(struct mg_connection *c, struct mg_http_message *hm) {
    /* v2 works the same as v1 for our purposes */
    eos_oauth_v1(c, hm);
}

void eos_sdk_accounts(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    mg_http_get_var(&hm->query, "accountId", account_id, sizeof(account_id));
    if (!account_id[0]) strncpy(account_id, "CyberPlayer", sizeof(account_id) - 1);

    cJSON *arr  = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "accountId",         account_id);
    cJSON_AddStringToObject(item, "displayName",       account_id);
    cJSON_AddStringToObject(item, "preferredLanguage", "en");
    cJSON_AddItemToObject  (item, "linkedAccounts",    cJSON_CreateArray());
    cJSON_AddBoolToObject  (item, "cabinedMode",       0);
    cJSON_AddBoolToObject  (item, "empty",             0);
    cJSON_AddItemToArray(arr, item);

    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void eos_sdk_config(struct mg_connection *c, struct mg_http_message *hm) {
    /* The EOS SDK config blob — only the fields the game actually reads.
       The full 500-line version from Neonite is replicated here but
       trimmed to what matters for Ch6/Ch7. */
    const char *config =
    "{"
      "\"client\":{"
        "\"BaseService\":{\"HttpRetryLimit\":4,\"HttpRetryResponseCodes\":[429,503,504]},"
        "\"HTTP\":{\"HttpReceiveTimeout\":30,\"bEnableHttp\":true,\"HttpTimeout\":30,"
          "\"HttpConnectionTimeout\":60,\"HttpSendTimeout\":30},"
        "\"XMPP\":{\"bEnabled\":true,\"bEnableWebsockets\":true,\"ThreadStackSize\":131072},"
        "\"Messaging.XMPP\":{"
          "\"ServerAddr\":\"wss://xmpp-service-prod.ol.epicgames.com\","
          "\"Domain\":\"prod.ol.epicgames.com\","
          "\"ServerPort\":443,"
          "\"PingInterval\":60,"
          "\"PingTimeout\":30,"
          "\"bPrivateChatFriendsOnly\":true"
        "},"
        "\"Messaging.EpicConnect\":{"
          "\"Url\":\"wss://connect.epicgames.dev\","
          "\"Urlv2\":\"wss://connect.epicgames.dev/v2\","
          "\"UseV2Connection\":false,"
          "\"ClientHeartbeatIntervalMilliseconds\":30000,"
          "\"RetryBackoffExponentialBase\":2,"
          "\"RetryBackoffMaxSeconds\":180"
        "},"
        "\"PresenceClient\":{"
          "\"bSetOfflineOnLogoutEnabled\":true,"
          "\"bSetOfflineOnShutdownEnabled\":true,"
          "\"PresenceAutoUpdateInSeconds\":600"
        "},"
        "\"/Script/Engine.NetworkSettings\":{\"n.VerifyPeer\":false},"
        "\"RateLimiter\":{\"MessageCount\":300,\"TimeIntervalInSeconds\":60},"
        "\"SessionsClient\":{\"HeartbeatIntervalSecs\":30},"
        "\"ConnectClient\":{\"MaxProductUserIdMappingsQueryUserIds\":128}"
      "},"
      "\"services\":{"
        "\"AccountsService\":{\"BaseUrl\":\"https://egp-idsoc-proxy-prod.ol.epicgames.com/account\"},"
        "\"FriendService\":{\"BaseUrl\":\"https://egp-idsoc-proxy-prod.ol.epicgames.com/friends\"},"
        "\"PartiesService\":{\"BaseUrl\":\"https://api.epicgames.dev/epic/party\"},"
        "\"MatchmakingService\":{\"BaseUrl\":\"https://api.epicgames.dev/epic/matchmaking\"}"
      "}"
    "}";

    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", config);
}

void eos_sdk_product(struct mg_connection *c, struct mg_http_message *hm) {
    eos_sdk_config(c, hm);
}

void eos_blocklist(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "[]");
}

void eos_presence(struct mg_connection *c, struct mg_http_message *hm) {
    /* Just acknowledge — no real presence tracking needed */
    mg_http_reply(c, 204, "", "");
}

void eos_chat_summary(struct mg_connection *c, struct mg_http_message *hm) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "conversations", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "settings",      cJSON_CreateObject());

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void eos_chat_conversations(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"conversations\":[],\"cursor\":null}");
}

void eos_telemetry(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 204, "", "");
}

void eos_datarouter(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 204, "", "");
}

void eos_product_users_search(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"elements\":[]}");
}
