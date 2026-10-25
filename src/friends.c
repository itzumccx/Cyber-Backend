#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "friends.h"
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

/* Extract the last path segment from a URI */
static void last_segment(const char *uri, size_t uri_len,
                          char *out, size_t out_len) {
    const char *end = uri + uri_len;
    const char *slash = uri;
    for (const char *p = uri; p < end; p++)
        if (*p == '/') slash = p;
    slash++;
    size_t n = (size_t)(end - slash);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, slash, n);
    out[n] = '\0';
}

/* Build a single friend object */
static cJSON *make_friend(const char *account_id) {
    char ts[64]; now_iso(ts, sizeof(ts));
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "accountId",  account_id);
    cJSON_AddStringToObject(f, "status",     "ACCEPTED");
    cJSON_AddStringToObject(f, "direction",  "OUTBOUND");
    cJSON_AddStringToObject(f, "created",    ts);
    cJSON_AddBoolToObject  (f, "favorite",   0);
    return f;
}

/* Build a summary friend object */
static cJSON *make_summary_friend(const char *account_id) {
    char ts[64]; now_iso(ts, sizeof(ts));
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "accountId", account_id);
    cJSON_AddItemToObject  (f, "groups",    cJSON_CreateArray());
    cJSON_AddNumberToObject(f, "mutual",    0);
    cJSON_AddStringToObject(f, "alias",     "");
    cJSON_AddStringToObject(f, "note",      "");
    cJSON_AddBoolToObject  (f, "favorite",  0);
    cJSON_AddStringToObject(f, "created",   ts);
    return f;
}

/* ------------------------------------------------------------------ */
/* Friends                                                              */
/* ------------------------------------------------------------------ */

void friends_list(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    last_segment(hm->uri.buf, hm->uri.len, account_id, sizeof(account_id));

    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, make_friend(account_id));

    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void friends_summary(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    /* URI: /friends/api/v1/:accountId/summary — accountId is segment 4 */
    const char *p = hm->uri.buf;
    int seg = 0;
    const char *start = NULL;
    for (size_t i = 0; i <= hm->uri.len; i++) {
        if (p[i] == '/' || p[i] == '\0') {
            if (seg == 4 && start) {
                size_t n = (p + i) - start;
                if (n >= sizeof(account_id)) n = sizeof(account_id) - 1;
                memcpy(account_id, start, n);
                account_id[n] = '\0';
                break;
            }
            seg++;
            start = p + i + 1;
        }
    }

    cJSON *root = cJSON_CreateObject();

    cJSON *friends_arr = cJSON_CreateArray();
    cJSON_AddItemToArray(friends_arr, make_summary_friend(account_id));
    cJSON_AddItemToObject(root, "friends", friends_arr);

    cJSON_AddItemToObject(root, "incoming",  cJSON_CreateArray());
    cJSON_AddItemToObject(root, "outgoing",  cJSON_CreateArray());
    cJSON_AddItemToObject(root, "suggested", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "blocklist", cJSON_CreateArray());

    cJSON *settings = cJSON_CreateObject();
    cJSON_AddStringToObject(settings, "acceptInvites", "public");
    cJSON_AddItemToObject(root, "settings", settings);

    cJSON *limits = cJSON_CreateObject();
    cJSON_AddBoolToObject(limits, "incoming", 0);
    cJSON_AddBoolToObject(limits, "outgoing", 0);
    cJSON_AddBoolToObject(limits, "accepted", 0);
    cJSON_AddItemToObject(root, "limitsReached", limits);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void friends_settings(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"acceptInvites\":\"public\"}");
}

void friends_blocklist(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"blockedUsers\":[]}");
}

void friends_recent(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "[]");
}

void friends_stats(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    last_segment(hm->uri.buf, hm->uri.len, account_id, sizeof(account_id));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "startTime", 1698908409);
    cJSON_AddNumberToObject(root, "endTime",   1735714808);
    cJSON_AddItemToObject  (root, "stats",     cJSON_CreateObject());
    cJSON_AddStringToObject(root, "accountId", account_id);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void friends_receipts(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "[]");
}

/* ------------------------------------------------------------------ */
/* Party / Lobby                                                        */
/* ------------------------------------------------------------------ */

void party_create(struct mg_connection *c, struct mg_http_message *hm) {
    char ts[64]; now_iso(ts, sizeof(ts));

    /* Parse body for join_info */
    char *raw = malloc(hm->body.len + 1);
    memcpy(raw, hm->body.buf, hm->body.len);
    raw[hm->body.len] = '\0';
    cJSON *body = cJSON_Parse(raw);
    free(raw);
    if (!body) body = cJSON_CreateObject();

    /* Generate a party ID */
    char party_id[32];
    snprintf(party_id, sizeof(party_id), "cyber%ld", (long)time(NULL));

    /* Try to extract accountId from join_info */
    char account_id[128] = "CyberPlayer";
    cJSON *join_info = cJSON_GetObjectItem(body, "join_info");
    cJSON *conn      = join_info ? cJSON_GetObjectItem(join_info, "connection") : NULL;
    cJSON *conn_id   = conn      ? cJSON_GetObjectItem(conn,      "id")         : NULL;
    if (cJSON_IsString(conn_id)) {
        /* id is like "accountId@xmpp..." */
        const char *at = strchr(conn_id->valuestring, '@');
        size_t n = at ? (size_t)(at - conn_id->valuestring) : strlen(conn_id->valuestring);
        if (n >= sizeof(account_id)) n = sizeof(account_id) - 1;
        memcpy(account_id, conn_id->valuestring, n);
        account_id[n] = '\0';
    }

    /* Build member */
    cJSON *member = cJSON_CreateObject();
    cJSON_AddStringToObject(member, "account_id", account_id);
    cJSON_AddStringToObject(member, "updated_at", ts);
    cJSON_AddStringToObject(member, "joined_at",  ts);
    cJSON_AddStringToObject(member, "role",       "CAPTAIN");
    cJSON_AddNumberToObject(member, "revision",   0);
    cJSON_AddItemToObject  (member, "connections", cJSON_CreateArray());
    cJSON_AddItemToObject  (member, "meta",        cJSON_CreateObject());

    cJSON *members = cJSON_CreateArray();
    cJSON_AddItemToArray(members, member);

    /* Config */
    cJSON *config = cJSON_GetObjectItem(body, "config");
    if (!config) { config = cJSON_CreateObject(); }
    else { config = cJSON_Duplicate(config, 1); }

    /* Meta */
    cJSON *meta = cJSON_GetObjectItem(body, "meta");
    if (!meta) { meta = cJSON_CreateObject(); }
    else { meta = cJSON_Duplicate(meta, 1); }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id",         party_id);
    cJSON_AddStringToObject(root, "created_at", ts);
    cJSON_AddStringToObject(root, "updated_at", ts);
    cJSON_AddItemToObject  (root, "config",     config);
    cJSON_AddItemToObject  (root, "members",    members);
    cJSON_AddItemToObject  (root, "applicants", cJSON_CreateArray());
    cJSON_AddItemToObject  (root, "meta",       meta);
    cJSON_AddItemToObject  (root, "invites",    cJSON_CreateArray());
    cJSON_AddNumberToObject(root, "revision",   0);

    char *rbody = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    cJSON_Delete(body);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", rbody);
    free(rbody);
}

void party_local(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"current\":[],\"pending\":[],\"invites\":[],\"pings\":[]}");
}

void party_get(struct mg_connection *c, struct mg_http_message *hm) {
    char party_id[128] = {0};
    last_segment(hm->uri.buf, hm->uri.len, party_id, sizeof(party_id));
    char ts[64]; now_iso(ts, sizeof(ts));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id",         party_id);
    cJSON_AddStringToObject(root, "created_at", ts);
    cJSON_AddStringToObject(root, "updated_at", ts);

    cJSON *config = cJSON_CreateObject();
    cJSON_AddStringToObject(config, "type",            "DEFAULT");
    cJSON_AddStringToObject(config, "joinability",     "OPEN");
    cJSON_AddStringToObject(config, "discoverability", "ALL");
    cJSON_AddStringToObject(config, "sub_type",        "default");
    cJSON_AddNumberToObject(config, "max_size",        16);
    cJSON_AddNumberToObject(config, "invite_ttl",      14400);
    cJSON_AddBoolToObject  (config, "join_confirmation",0);
    cJSON_AddItemToObject  (root, "config", config);

    cJSON_AddItemToObject(root, "members",    cJSON_CreateArray());
    cJSON_AddItemToObject(root, "applicants", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "meta",       cJSON_CreateObject());
    cJSON_AddItemToObject(root, "invites",    cJSON_CreateArray());
    cJSON_AddNumberToObject(root, "revision", 0);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void party_join(struct mg_connection *c, struct mg_http_message *hm) {
    /* Extract partyId — it's the segment before /members/ */
    char party_id[128] = {0};
    const char *p = hm->uri.buf;
    const char *members_tag = strstr(p, "/members/");
    if (members_tag) {
        const char *slash = members_tag - 1;
        while (slash > p && *slash != '/') slash--;
        slash++;
        size_t n = (size_t)(members_tag - slash);
        if (n >= sizeof(party_id)) n = sizeof(party_id) - 1;
        memcpy(party_id, slash, n);
        party_id[n] = '\0';
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status",   "JOINED");
    cJSON_AddStringToObject(root, "party_id", party_id);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void party_meta(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 204, "", "");
}

void party_pings(struct mg_connection *c, struct mg_http_message *hm) {
    char ts[64]; now_iso(ts, sizeof(ts));
    /* Extract pinger/account from URI segments */
    char pinger_id[128]  = "unknown";
    char account_id[128] = "unknown";
    /* URI: /party/api/v1/{ns}/user/{accountId}/pings/{pingerId} */
    const char *pings_tag = strstr(hm->uri.buf, "/pings/");
    if (pings_tag) {
        pings_tag += 7;
        size_t n = hm->uri.len - (pings_tag - hm->uri.buf);
        if (n >= sizeof(pinger_id)) n = sizeof(pinger_id) - 1;
        memcpy(pinger_id, pings_tag, n);
        pinger_id[n] = '\0';
    }

    /* accountId is between /user/ and /pings/ */
    const char *user_tag  = strstr(hm->uri.buf, "/user/");
    if (user_tag && pings_tag) {
        user_tag += 6;
        const char *end = strstr(user_tag, "/pings/");
        if (end) {
            size_t n = (size_t)(end - user_tag);
            if (n >= sizeof(account_id)) n = sizeof(account_id) - 1;
            memcpy(account_id, user_tag, n);
            account_id[n] = '\0';
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "sent_by",    pinger_id);
    cJSON_AddStringToObject(root, "sent_to",    account_id);
    cJSON_AddStringToObject(root, "sent_at",    ts);
    cJSON_AddStringToObject(root, "expires_at", "9999-12-31T23:59:59.999Z");
    cJSON_AddItemToObject  (root, "meta",       cJSON_CreateObject());

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void party_privacy(struct mg_connection *c, struct mg_http_message *hm) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"receiveInvites\":\"ALL\",\"receiveIntentions\":\"ALL\"}");
}
