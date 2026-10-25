#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "locker.h"
#include "profile.h"
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

/* Extract accountId from locker URIs.
   Pattern: /api/locker/v{3|4}/:deploymentId/account/:accountId/... */
static void locker_account_id(const char *uri, char *out, size_t out_len) {
    out[0] = '\0';
    const char *tag = strstr(uri, "/account/");
    if (!tag) return;
    tag += 9; /* skip "/account/" */
    const char *end = strchr(tag, '/');
    size_t n = end ? (size_t)(end - tag) : strlen(tag);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, tag, n);
    out[n] = '\0';
}

/* Generate a simple UUID-like string */
static void make_id(char *out, size_t len) {
    snprintf(out, len, "cyber%08lx%04lx", (unsigned long)time(NULL),
             (unsigned long)(time(NULL) & 0xFFFF));
}

/* Parse body JSON helper */
static cJSON *parse_body(struct mg_http_message *hm) {
    if (hm->body.len == 0) return cJSON_CreateObject();
    char *raw = malloc(hm->body.len + 1);
    memcpy(raw, hm->body.buf, hm->body.len);
    raw[hm->body.len] = '\0';
    cJSON *j = cJSON_Parse(raw);
    free(raw);
    return j ? j : cJSON_CreateObject();
}

/* Get or create a locker profile.
   Returns a cJSON object the caller must cJSON_Delete(). */
static cJSON *get_or_create_locker(const char *account_id, int version) {
    cJSON *locker = profile_read_locker(account_id, version);
    if (locker) return locker;

    /* Try template */
    locker = profile_read_locker_template(version);
    if (!locker) {
        /* Build a minimal empty locker */
        locker = cJSON_CreateObject();
        cJSON_AddStringToObject(locker, "accountId",   account_id);
        if (version == 3) {
            cJSON_AddItemToObject(locker, "activeLoadouts",  cJSON_CreateArray());
            cJSON_AddItemToObject(locker, "loadoutPresets",  cJSON_CreateArray());
        } else {
            cJSON *alg = cJSON_CreateObject();
            char ts[64]; now_iso(ts, sizeof(ts));
            cJSON_AddStringToObject(alg, "accountId",    account_id);
            cJSON_AddStringToObject(alg, "creationTime", ts);
            cJSON_AddStringToObject(alg, "updatedTime",  ts);
            cJSON_AddItemToObject  (alg, "loadouts",     cJSON_CreateObject());
            cJSON_AddItemToObject  (locker, "activeLoadoutGroup",  alg);
            cJSON_AddItemToObject  (locker, "loadoutGroupPresets", cJSON_CreateArray());
            cJSON_AddItemToObject  (locker, "loadoutPresets",      cJSON_CreateArray());
        }
    } else {
        /* Stamp account info */
        char ts[64]; now_iso(ts, sizeof(ts));
        if (version == 3) {
            cJSON *al;
            cJSON_ArrayForEach(al, cJSON_GetObjectItem(locker, "activeLoadouts")) {
                cJSON_DeleteItemFromObject(al, "accountId");
                cJSON_DeleteItemFromObject(al, "creationTime");
                cJSON_AddStringToObject(al, "accountId",    account_id);
                cJSON_AddStringToObject(al, "creationTime", ts);
            }
        } else {
            cJSON *alg = cJSON_GetObjectItem(locker, "activeLoadoutGroup");
            if (alg) {
                cJSON_DeleteItemFromObject(alg, "accountId");
                cJSON_DeleteItemFromObject(alg, "creationTime");
                cJSON_DeleteItemFromObject(alg, "updatedTime");
                cJSON_AddStringToObject(alg, "accountId",    account_id);
                cJSON_AddStringToObject(alg, "creationTime", ts);
                cJSON_AddStringToObject(alg, "updatedTime",  ts);
            }
        }
    }

    profile_save_locker(account_id, version, locker);
    cyber_log("Created lockerv%d for %s", version, account_id);
    return locker;
}

/* ------------------------------------------------------------------ */
/* v3 handlers                                                          */
/* ------------------------------------------------------------------ */

void locker_v3_get(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    locker_account_id(hm->uri.buf, account_id, sizeof(account_id));

    cJSON *locker = get_or_create_locker(account_id, 3);
    char *body = cJSON_PrintUnformatted(locker);
    cJSON_Delete(locker);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void locker_v3_set_loadout(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    locker_account_id(hm->uri.buf, account_id, sizeof(account_id));

    /* loadoutType is between /loadout/ and /account/ */
    char loadout_type[64] = {0};
    const char *lt = strstr(hm->uri.buf, "/loadout/");
    if (lt) {
        lt += 9;
        const char *end = strstr(lt, "/account/");
        if (end) {
            size_t n = (size_t)(end - lt);
            if (n >= sizeof(loadout_type)) n = sizeof(loadout_type) - 1;
            memcpy(loadout_type, lt, n);
            loadout_type[n] = '\0';
        }
    }

    cJSON *body   = parse_body(hm);
    cJSON *locker = get_or_create_locker(account_id, 3);

    /* Find the activeLoadout matching loadout_type */
    cJSON *active_loadouts = cJSON_GetObjectItem(locker, "activeLoadouts");
    cJSON *al;
    cJSON_ArrayForEach(al, active_loadouts) {
        cJSON *lt_field = cJSON_GetObjectItem(al, "loadoutType");
        if (cJSON_IsString(lt_field) &&
            strcmp(lt_field->valuestring, loadout_type) == 0) {
            cJSON *slots = cJSON_GetObjectItem(body, "loadoutSlots");
            if (slots) {
                cJSON_DeleteItemFromObject(al, "loadoutSlots");
                cJSON_AddItemToObject(al, "loadoutSlots", cJSON_Duplicate(slots, 1));
            }
            char ts[64]; now_iso(ts, sizeof(ts));
            cJSON_DeleteItemFromObject(al, "updatedTime");
            cJSON_AddStringToObject(al, "updatedTime", ts);
            break;
        }
    }

    profile_save_locker(account_id, 3, locker);

    cJSON *slot_resp = cJSON_CreateObject();
    cJSON_AddStringToObject(slot_resp, "accountId",   account_id);
    cJSON_AddStringToObject(slot_resp, "loadoutType", loadout_type);
    char ts[64]; now_iso(ts, sizeof(ts));
    cJSON_AddStringToObject(slot_resp, "updatedTime", ts);
    cJSON *slots_r = cJSON_GetObjectItem(body, "loadoutSlots");
    if (slots_r) cJSON_AddItemToObject(slot_resp, "loadoutSlots", cJSON_Duplicate(slots_r, 1));
    else cJSON_AddItemToObject(slot_resp, "loadoutSlots", cJSON_CreateArray());

    char *rbody = cJSON_PrintUnformatted(slot_resp);
    cJSON_Delete(slot_resp);
    cJSON_Delete(locker);
    cJSON_Delete(body);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", rbody);
    free(rbody);
}

void locker_v3_preset(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    locker_account_id(hm->uri.buf, account_id, sizeof(account_id));

    /* presetIndex is the last path segment */
    char idx_str[16] = "0";
    const char *idx_tag = strstr(hm->uri.buf, "/index/");
    if (idx_tag) { idx_tag += 7; strncpy(idx_str, idx_tag, sizeof(idx_str) - 1); }
    int preset_index = atoi(idx_str);

    /* loadoutType */
    char loadout_type[64] = {0};
    const char *lt = strstr(hm->uri.buf, "/loadout/");
    if (lt) {
        lt += 9;
        const char *end = strstr(lt, "/account/");
        if (end) { size_t n = (size_t)(end - lt); if (n >= sizeof(loadout_type)) n = sizeof(loadout_type)-1; memcpy(loadout_type, lt, n); loadout_type[n]='\0'; }
    }

    cJSON *body   = parse_body(hm);
    cJSON *locker = get_or_create_locker(account_id, 3);
    cJSON *presets = cJSON_GetObjectItem(locker, "loadoutPresets");
    char ts[64]; now_iso(ts, sizeof(ts));

    /* Find or create preset */
    cJSON *found = NULL;
    cJSON *preset;
    cJSON_ArrayForEach(preset, presets) {
        cJSON *pi = cJSON_GetObjectItem(preset, "presetIndex");
        if (pi && pi->valueint == preset_index) { found = preset; break; }
    }

    char preset_id[64]; make_id(preset_id, sizeof(preset_id));
    cJSON *display = cJSON_GetObjectItem(body, "displayName");
    cJSON *slots   = cJSON_GetObjectItem(body, "loadoutSlots");
    cJSON *ath_id  = cJSON_GetObjectItem(body, "athenaItemId");

    if (found) {
        if (ath_id)  { cJSON_DeleteItemFromObject(found, "athenaItemId");  cJSON_AddItemToObject(found, "athenaItemId",  cJSON_Duplicate(ath_id,  1)); }
        if (slots)   { cJSON_DeleteItemFromObject(found, "loadoutSlots");  cJSON_AddItemToObject(found, "loadoutSlots",  cJSON_Duplicate(slots,   1)); }
        if (display) { cJSON_DeleteItemFromObject(found, "displayName");   cJSON_AddStringToObject(found, "displayName", display->valuestring); }
        cJSON_DeleteItemFromObject(found, "updatedTime");
        cJSON_AddStringToObject(found, "updatedTime", ts);
    } else {
        cJSON *np = cJSON_CreateObject();
        cJSON_AddStringToObject(np, "accountId",           account_id);
        cJSON_AddStringToObject(np, "loadoutType",         loadout_type);
        cJSON_AddStringToObject(np, "presetId",            preset_id);
        cJSON_AddNumberToObject(np, "presetIndex",         preset_index);
        cJSON_AddItemToObject  (np, "athenaItemId",        ath_id  ? cJSON_Duplicate(ath_id, 1) : cJSON_CreateNull());
        cJSON_AddStringToObject(np, "creationTime",        ts);
        cJSON_AddStringToObject(np, "updatedTime",         ts);
        cJSON_AddItemToObject  (np, "loadoutSlots",        slots   ? cJSON_Duplicate(slots, 1) : cJSON_CreateArray());
        cJSON_AddStringToObject(np, "displayName",         display ? display->valuestring : "");
        cJSON_AddStringToObject(np, "presetFavoriteStatus","EMPTY");
        cJSON_AddItemToArray(presets, np);
        found = np;
    }

    profile_save_locker(account_id, 3, locker);

    char *rbody = cJSON_PrintUnformatted(found);
    cJSON_Delete(locker);
    cJSON_Delete(body);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", rbody);
    free(rbody);
}

/* ------------------------------------------------------------------ */
/* v4 handlers                                                          */
/* ------------------------------------------------------------------ */

void locker_v4_get(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    locker_account_id(hm->uri.buf, account_id, sizeof(account_id));

    cJSON *locker = get_or_create_locker(account_id, 4);
    char *body = cJSON_PrintUnformatted(locker);
    cJSON_Delete(locker);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

void locker_v4_set_loadout(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    locker_account_id(hm->uri.buf, account_id, sizeof(account_id));

    cJSON *body   = parse_body(hm);
    cJSON *locker = get_or_create_locker(account_id, 4);
    cJSON *alg    = cJSON_GetObjectItem(locker, "activeLoadoutGroup");

    if (alg) {
        cJSON *loadouts = cJSON_GetObjectItem(body, "loadouts");
        if (loadouts) {
            cJSON_DeleteItemFromObject(alg, "loadouts");
            cJSON_AddItemToObject(alg, "loadouts", cJSON_Duplicate(loadouts, 1));
        }
        cJSON *preset_id = cJSON_GetObjectItem(body, "equippedPresetId");
        if (preset_id) {
            cJSON_DeleteItemFromObject(alg, "equippedPresetId");
            cJSON_AddItemToObject(alg, "equippedPresetId", cJSON_Duplicate(preset_id, 1));
        }
        char ts[64]; now_iso(ts, sizeof(ts));
        cJSON_DeleteItemFromObject(alg, "updatedTime");
        cJSON_AddStringToObject(alg, "updatedTime", ts);
    }

    profile_save_locker(account_id, 4, locker);

    char *rbody = cJSON_PrintUnformatted(alg ? alg : locker);
    cJSON_Delete(locker);
    cJSON_Delete(body);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", rbody);
    free(rbody);
}

void locker_v4_group_preset(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    locker_account_id(hm->uri.buf, account_id, sizeof(account_id));

    char idx_str[16] = "0";
    const char *idx_tag = strstr(hm->uri.buf, "/index/");
    if (idx_tag) { idx_tag += 7; strncpy(idx_str, idx_tag, sizeof(idx_str) - 1); }
    int preset_index = atoi(idx_str);

    cJSON *body    = parse_body(hm);
    cJSON *locker  = get_or_create_locker(account_id, 4);
    cJSON *presets = cJSON_GetObjectItem(locker, "loadoutGroupPresets");
    char ts[64]; now_iso(ts, sizeof(ts));
    char preset_id[64]; make_id(preset_id, sizeof(preset_id));

    cJSON *display  = cJSON_GetObjectItem(body, "displayName");
    cJSON *loadouts = cJSON_GetObjectItem(body, "loadouts");
    cJSON *ath_id   = cJSON_GetObjectItem(body, "athenaItemId");

    cJSON *found = NULL;
    cJSON *preset;
    cJSON_ArrayForEach(preset, presets) {
        cJSON *pi = cJSON_GetObjectItem(preset, "presetIndex");
        if (pi && pi->valueint == preset_index) { found = preset; break; }
    }

    if (found) {
        if (ath_id)   { cJSON_DeleteItemFromObject(found, "athenaItemId"); cJSON_AddItemToObject(found, "athenaItemId", cJSON_Duplicate(ath_id, 1)); }
        if (loadouts) { cJSON_DeleteItemFromObject(found, "loadouts");     cJSON_AddItemToObject(found, "loadouts",     cJSON_Duplicate(loadouts, 1)); }
        if (display)  { cJSON_DeleteItemFromObject(found, "displayName");  cJSON_AddStringToObject(found, "displayName", display->valuestring); }
        cJSON_DeleteItemFromObject(found, "updatedTime");
        cJSON_AddStringToObject(found, "updatedTime", ts);
    } else {
        cJSON *np = cJSON_CreateObject();
        cJSON_AddStringToObject(np, "accountId",           account_id);
        cJSON_AddItemToObject  (np, "athenaItemId",        ath_id   ? cJSON_Duplicate(ath_id,   1) : cJSON_CreateNull());
        cJSON_AddStringToObject(np, "creationTime",        ts);
        cJSON_AddStringToObject(np, "displayName",         display  ? display->valuestring : "");
        cJSON_AddItemToObject  (np, "loadouts",            loadouts ? cJSON_Duplicate(loadouts,1) : cJSON_CreateObject());
        cJSON_AddStringToObject(np, "presetFavoriteStatus","EMPTY");
        cJSON_AddStringToObject(np, "presetId",            preset_id);
        cJSON_AddNumberToObject(np, "presetIndex",         preset_index);
        cJSON_AddStringToObject(np, "updatedTime",         ts);
        cJSON_AddItemToArray(presets, np);
        found = np;
    }

    profile_save_locker(account_id, 4, locker);

    char *rbody = cJSON_PrintUnformatted(found);
    cJSON_Delete(locker);
    cJSON_Delete(body);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", rbody);
    free(rbody);
}

void locker_v4_preset(struct mg_connection *c, struct mg_http_message *hm) {
    /* Same logic as v3 preset but stored in loadoutPresets of the v4 file */
    char account_id[128] = {0};
    locker_account_id(hm->uri.buf, account_id, sizeof(account_id));

    char idx_str[16] = "0";
    const char *idx_tag = strstr(hm->uri.buf, "/index/");
    if (idx_tag) { idx_tag += 7; strncpy(idx_str, idx_tag, sizeof(idx_str) - 1); }
    int preset_index = atoi(idx_str);

    char loadout_type[64] = {0};
    const char *lt = strstr(hm->uri.buf, "/loadout/");
    if (lt) {
        lt += 9;
        const char *end = strstr(lt, "/account/");
        if (end) { size_t n = (size_t)(end - lt); if (n >= sizeof(loadout_type)) n = sizeof(loadout_type)-1; memcpy(loadout_type, lt, n); loadout_type[n]='\0'; }
    }

    cJSON *body    = parse_body(hm);
    cJSON *locker  = get_or_create_locker(account_id, 4);
    cJSON *presets = cJSON_GetObjectItem(locker, "loadoutPresets");
    char ts[64]; now_iso(ts, sizeof(ts));
    char preset_id[64]; make_id(preset_id, sizeof(preset_id));

    cJSON *display = cJSON_GetObjectItem(body, "displayName");
    cJSON *slots   = cJSON_GetObjectItem(body, "loadoutSlots");
    cJSON *ath_id  = cJSON_GetObjectItem(body, "athenaItemId");

    cJSON *found = NULL;
    cJSON *preset;
    cJSON_ArrayForEach(preset, presets) {
        cJSON *pi  = cJSON_GetObjectItem(preset, "presetIndex");
        cJSON *plt = cJSON_GetObjectItem(preset, "loadoutType");
        if (pi && pi->valueint == preset_index &&
            cJSON_IsString(plt) && strcmp(plt->valuestring, loadout_type) == 0) {
            found = preset; break;
        }
    }

    if (found) {
        if (ath_id)  { cJSON_DeleteItemFromObject(found, "athenaItemId"); cJSON_AddItemToObject(found, "athenaItemId", cJSON_Duplicate(ath_id, 1)); }
        if (slots)   { cJSON_DeleteItemFromObject(found, "loadoutSlots"); cJSON_AddItemToObject(found, "loadoutSlots", cJSON_Duplicate(slots,  1)); }
        if (display) { cJSON_DeleteItemFromObject(found, "displayName");  cJSON_AddStringToObject(found, "displayName", display->valuestring); }
        cJSON_DeleteItemFromObject(found, "updatedTime");
        cJSON_AddStringToObject(found, "updatedTime", ts);
    } else {
        cJSON *np = cJSON_CreateObject();
        cJSON_AddStringToObject(np, "accountId",           account_id);
        cJSON_AddStringToObject(np, "loadoutType",         loadout_type);
        cJSON_AddStringToObject(np, "presetId",            preset_id);
        cJSON_AddNumberToObject(np, "presetIndex",         preset_index);
        cJSON_AddItemToObject  (np, "athenaItemId",        ath_id  ? cJSON_Duplicate(ath_id, 1) : cJSON_CreateNull());
        cJSON_AddStringToObject(np, "creationTime",        ts);
        cJSON_AddStringToObject(np, "updatedTime",         ts);
        cJSON_AddItemToObject  (np, "loadoutSlots",        slots   ? cJSON_Duplicate(slots, 1) : cJSON_CreateArray());
        cJSON_AddStringToObject(np, "displayName",         display ? display->valuestring : "");
        cJSON_AddStringToObject(np, "presetFavoriteStatus","EMPTY");
        cJSON_AddItemToArray(presets, np);
        found = np;
    }

    profile_save_locker(account_id, 4, locker);

    char *rbody = cJSON_PrintUnformatted(found);
    cJSON_Delete(locker);
    cJSON_Delete(body);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", rbody);
    free(rbody);
}

void locker_v4_immutable(struct mg_connection *c, struct mg_http_message *hm) {
    /* Lock in variant for a companion item */
    char account_id[128] = {0};
    locker_account_id(hm->uri.buf, account_id, sizeof(account_id));

    /* companion UUID is the last segment */
    char companion[128] = {0};
    const char *lock_tag = strstr(hm->uri.buf, "/lock-in-immutable-item/");
    if (lock_tag) {
        lock_tag += 24;
        /* UUID is after the last colon if present */
        const char *colon = strrchr(lock_tag, ':');
        const char *start = colon ? colon + 1 : lock_tag;
        size_t n = (hm->uri.buf + hm->uri.len) - start;
        if (n >= sizeof(companion)) n = sizeof(companion) - 1;
        memcpy(companion, start, n);
        companion[n] = '\0';
    }

    cJSON *body    = parse_body(hm);
    cJSON *athena  = profile_read(account_id, "athena");

    if (athena && companion[0]) {
        cJSON *items     = cJSON_GetObjectItem(athena, "items");
        cJSON *comp_item = items ? cJSON_GetObjectItem(items, companion) : NULL;
        if (comp_item) {
            cJSON *attrs    = cJSON_GetObjectItem(comp_item, "attributes");
            if (!attrs) { attrs = cJSON_CreateObject(); cJSON_AddItemToObject(comp_item, "attributes", attrs); }
            cJSON_DeleteItemFromObject(attrs, "locked_in");
            cJSON_AddBoolToObject(attrs, "locked_in", 1);

            cJSON *variants  = cJSON_GetObjectItem(attrs, "variants");
            cJSON *bvariants = cJSON_GetObjectItem(body, "variants");
            if (variants && cJSON_IsObject(bvariants)) {
                cJSON *channel_entry;
                cJSON_ArrayForEach(channel_entry, variants) {
                    cJSON *ch_name = cJSON_GetObjectItem(channel_entry, "channel");
                    if (!cJSON_IsString(ch_name)) continue;
                    cJSON *update = cJSON_GetObjectItem(bvariants, ch_name->valuestring);
                    if (!update) continue;
                    cJSON *vtag = cJSON_GetObjectItem(update, "variantTag");
                    if (cJSON_IsString(vtag)) {
                        cJSON_DeleteItemFromObject(channel_entry, "active");
                        cJSON_AddStringToObject(channel_entry, "active", vtag->valuestring);
                    }
                }
            }
            profile_save(account_id, "athena", athena);
        }
        cJSON_Delete(athena);
    }

    cJSON_Delete(body);
    mg_http_reply(c, 204, "", "");
}

void locker_v4_companion_name(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    locker_account_id(hm->uri.buf, account_id, sizeof(account_id));

    cJSON *body    = parse_body(hm);
    cJSON *athena  = profile_read(account_id, "athena");

    cJSON *cosmetic_id = cJSON_GetObjectItem(body, "cosmeticItemId");
    cJSON *new_name    = cJSON_GetObjectItem(body, "companionName");

    if (athena && cJSON_IsString(cosmetic_id) && cJSON_IsString(new_name)) {
        /* UUID is after the last colon */
        const char *uuid = strrchr(cosmetic_id->valuestring, ':');
        uuid = uuid ? uuid + 1 : cosmetic_id->valuestring;

        cJSON *items     = cJSON_GetObjectItem(athena, "items");
        cJSON *comp_item = items ? cJSON_GetObjectItem(items, uuid) : NULL;
        if (comp_item) {
            cJSON *attrs    = cJSON_GetObjectItem(comp_item, "attributes");
            cJSON *variants = attrs ? cJSON_GetObjectItem(attrs, "variants") : NULL;
            if (variants) {
                cJSON *v;
                cJSON_ArrayForEach(v, variants) {
                    cJSON *ch = cJSON_GetObjectItem(v, "channel");
                    if (cJSON_IsString(ch) && strcmp(ch->valuestring, "CustomName") == 0) {
                        cJSON_DeleteItemFromObject(v, "active");
                        cJSON_AddStringToObject(v, "active", new_name->valuestring);
                        break;
                    }
                }
            }
            profile_save(account_id, "athena", athena);
        }
        cJSON_Delete(athena);
    }

    cJSON_Delete(body);
    mg_http_reply(c, 204, "", "");
}

void locker_v4_cosmetic_data(struct mg_connection *c, struct mg_http_message *hm) {
    char account_id[128] = {0};
    locker_account_id(hm->uri.buf, account_id, sizeof(account_id));

    cJSON *data = profile_read(account_id, "cosmetic_data");
    if (!data) {
        data = profile_read_template("cosmetic_data");
        if (!data) data = cJSON_CreateObject();
        else {
            profile_save(account_id, "cosmetic_data", data);
        }
    }

    /* Remove nextToken to stop spamming requests */
    cJSON_DeleteItemFromObject(data, "nextToken");

    char ts[64]; now_iso(ts, sizeof(ts));
    cJSON_DeleteItemFromObject(data, "requestTime");
    cJSON_AddStringToObject(data, "requestTime", ts);

    char *body = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}
