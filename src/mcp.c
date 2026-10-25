#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mcp.h"
#include "profile.h"
#include "errors.h"
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

/* Extract a path segment by position (0-indexed from the first '/').
   /fortnite/api/game/v2/profile/{accountId}/client/{command}
   segment 5 = accountId, segment 7 = command                        */
static void uri_segment(const char *uri, int index, char *out, size_t out_len) {
    const char *p = uri;
    int seg = -1;
    const char *start = NULL;
    out[0] = '\0';
    while (*p) {
        if (*p == '/') {
            if (start && seg == index) {
                size_t n = p - start;
                if (n >= out_len) n = out_len - 1;
                memcpy(out, start, n);
                out[n] = '\0';
                return;
            }
            seg++;
            start = p + 1;
        }
        p++;
    }
    /* last segment */
    if (start && seg == index) {
        size_t n = strlen(start);
        if (n >= out_len) n = out_len - 1;
        memcpy(out, start, n);
        out[n] = '\0';
    }
}

/* Send the final MCP JSON response. Takes ownership of nothing — all
   cJSON objects passed in are still owned by the caller. */
static void send_mcp_response(struct mg_connection *c,
                               const char *profile_id,
                               cJSON *profile,
                               cJSON *changes,
                               cJSON *multi_update,
                               cJSON *notifications) {
    cJSON *root = cJSON_CreateObject();

    cJSON *rvn = cJSON_GetObjectItem(profile, "rvn");
    cJSON *cr  = cJSON_GetObjectItem(profile, "commandRevision");
    int rvn_val = rvn ? rvn->valueint : 1;
    int cr_val  = cr  ? cr->valueint  : 1;

    cJSON_AddNumberToObject(root, "profileRevision",          rvn_val);
    cJSON_AddStringToObject(root, "profileId",                profile_id);
    cJSON_AddNumberToObject(root, "profileChangesBaseRevision", rvn_val);
    cJSON_AddItemToObject  (root, "profileChanges",
                            changes ? cJSON_Duplicate(changes, 1) : cJSON_CreateArray());

    char ts[64]; now_iso(ts, sizeof(ts));
    cJSON_AddStringToObject(root, "serverTime",               ts);
    cJSON_AddNumberToObject(root, "profileCommandRevision",   cr_val);
    cJSON_AddNumberToObject(root, "responseVersion",          1);

    if (multi_update)
        cJSON_AddItemToObject(root, "multiUpdate", cJSON_Duplicate(multi_update, 1));
    if (notifications)
        cJSON_AddItemToObject(root, "notifications", cJSON_Duplicate(notifications, 1));

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

/* Get or create a profile. If it doesn't exist on disk, copies the
   template and writes it for this account. Returns NULL on total failure. */
static cJSON *get_or_create(const char *account_id, const char *profile_id) {
    cJSON *p = profile_read(account_id, profile_id);
    if (p) return p;

    p = profile_read_template(profile_id);
    if (!p) return NULL;

    /* Stamp it with account info */
    char ts[64]; now_iso(ts, sizeof(ts));
    cJSON_DeleteItemFromObject(p, "created");
    cJSON_DeleteItemFromObject(p, "updated");
    cJSON_DeleteItemFromObject(p, "accountId");
    cJSON_AddStringToObject(p, "created",   ts);
    cJSON_AddStringToObject(p, "updated",   ts);
    cJSON_AddStringToObject(p, "accountId", account_id);

    profile_save(account_id, profile_id, p);
    cyber_log("Created %s profile for %s", profile_id, account_id);
    return p;
}

/* Build a fullProfileUpdate change entry */
static cJSON *full_update(cJSON *profile) {
    cJSON *arr    = cJSON_CreateArray();
    cJSON *change = cJSON_CreateObject();
    cJSON_AddStringToObject(change, "changeType", "fullProfileUpdate");
    cJSON_AddItemToObject  (change, "profile", cJSON_Duplicate(profile, 1));
    cJSON_AddItemToArray(arr, change);
    return arr;
}

/* ------------------------------------------------------------------ */
/* MCP command dispatcher                                               */
/* ------------------------------------------------------------------ */

void mcp_handler(struct mg_connection *c, struct mg_http_message *hm) {
    /* Parse URI: /fortnite/api/game/v2/profile/{accountId}/client/{command} */
    char uri[512] = {0};
    size_t n = hm->uri.len < sizeof(uri) - 1 ? hm->uri.len : sizeof(uri) - 1;
    memcpy(uri, hm->uri.buf, n);
    uri[n] = '\0';

    char account_id[128] = {0};
    char command[128]    = {0};
    uri_segment(uri, 5, account_id, sizeof(account_id));
    uri_segment(uri, 7, command,    sizeof(command));

    /* profileId comes from query string */
    char profile_id[64] = "common_core";
    mg_http_get_var(&hm->query, "profileId", profile_id, sizeof(profile_id));

    /* Parse request body */
    cJSON *body = NULL;
    if (hm->body.len > 0) {
        char *raw = malloc(hm->body.len + 1);
        memcpy(raw, hm->body.buf, hm->body.len);
        raw[hm->body.len] = '\0';
        body = cJSON_Parse(raw);
        free(raw);
    }
    if (!body) body = cJSON_CreateObject();

    cyber_log("MCP %s -> %s (profile: %s)", account_id, command, profile_id);

    /* Always make sure the athena profile exists */
    cJSON *athena = get_or_create(account_id, "athena");
    cJSON *profile = get_or_create(account_id, profile_id);

    if (!profile) {
        cyber_send_error(c, 200, ERR_PROFILE_FORBIDDEN,
                         "Unable to find template for profile", 12813);
        cJSON_Delete(body);
        if (athena) cJSON_Delete(athena);
        return;
    }

    cJSON *changes       = cJSON_CreateArray();
    cJSON *multi_update  = NULL;
    cJSON *notifications = NULL;

    /* ---- QueryProfile -------------------------------------------- */
    if (strcmp(command, "QueryProfile") == 0) {

        /* ── Athena profile enrichment (like Neonite) ─────────────── */
        if (strcmp(profile_id, "athena") == 0) {
            cJSON *stats = cJSON_GetObjectItem(profile, "stats");
            cJSON *attrs = stats ? cJSON_GetObjectItem(stats, "attributes") : NULL;
            if (!attrs) {
                if (!stats) { stats = cJSON_CreateObject(); cJSON_AddItemToObject(profile, "stats", stats); }
                attrs = cJSON_CreateObject(); cJSON_AddItemToObject(stats, "attributes", attrs);
            }

            /* ── Detect game version from User-Agent ────────────── */
            int vg = 1;
            float version_f = 1.0f;
            {
                struct mg_str *ua = mg_http_get_header(hm, "User-Agent");
                if (ua && ua->buf) {
                    const char *rel = NULL;
                    for (size_t i = 0; i + 8 <= ua->len; i++) {
                        if (strncmp(ua->buf + i, "Release-", 8) == 0) {
                            rel = ua->buf + i + 8; break;
                        }
                    }
                    if (rel) {
                        vg = atoi(rel);
                        version_f = (float)atof(rel);
                        if (vg <= 0) vg = 1;
                    }
                }
            }

            /* ── season_num ─────────────────────────────────────── */
            {
                cJSON *val = cJSON_CreateNumber(vg);
                profile_modify_stat(profile, "season_num", val, NULL);
                cJSON_Delete(val);
            }

            /* ── favorite_character default ─────────────────────── */
            {
                cJSON *fc = cJSON_GetObjectItem(attrs, "favorite_character");
                if (!fc || !cJSON_IsString(fc) || !fc->valuestring[0]) {
                    cJSON *val = cJSON_CreateString("AthenaCharacter:CID_001_Athena_Commando_F_Default");
                    profile_modify_stat(profile, "favorite_character", val, NULL);
                    cJSON_Delete(val);
                }
            }

            /* ── past_seasons (season history) ──────────────────── */
            {
                cJSON *ps = cJSON_CreateArray();
                for (int s = 1; s <= 100; s++) {
                    cJSON *se = cJSON_CreateObject();
                    cJSON_AddNumberToObject(se, "seasonNumber",  s);
                    cJSON_AddNumberToObject(se, "numWins",       10000);
                    cJSON_AddNumberToObject(se, "seasonXp",      1000000);
                    cJSON_AddNumberToObject(se, "seasonLevel",   500);
                    cJSON_AddNumberToObject(se, "bookXp",        1000000);
                    cJSON_AddNumberToObject(se, "bookLevel",     500);
                    cJSON_AddBoolToObject  (se, "purchasedVIP",  1);
                    cJSON_AddItemToArray(ps, se);
                }
                profile_modify_stat(profile, "past_seasons", ps, NULL);
                cJSON_Delete(ps);
            }

            /* ── Season pass items (AthenaSeason) ──────────────── */
            if (vg >= 33) {
                const char *season_passes[] = {
                    "athenaseason%d",
                    "figmentpass_s01", "figmentpass_s02", "figmentpass_s03",
                    "figmentpass_s04",
                    "figment_s06_passdata_seasonasset",
                    "figment_s07_passdata_seasonasset",
                    "figment_s08_passdata_seasonasset",
                    "junoseason1pass", "junoseason2pass",
                    "s3pass_seasonasset", "s04pass_seasonasset",
                    "junoseason05pass_seasonasset",
                    "junoseason06pass_seasonasset",
                    "junoseason07pass_seasonasset",
                    "sparks_season07_seasonasset",
                    "sparks_season08_seasonasset",
                    "musicpasss09_seasonasset",
                    "musicpasss11_seasonasset",
                    "musicpasss12_seasonasset",
                    "musicpasss14_seasonasset",
                    "safflower_seasondata_seasonasset",
                    NULL
                };
                cJSON *items = cJSON_GetObjectItem(profile, "items");
                for (int i = 0; season_passes[i]; i++) {
                    char key[128], tid[128];
                    if (strstr(season_passes[i], "%d")) {
                        snprintf(key, sizeof(key), "AthenaSeason:athenaseason%d", vg);
                        snprintf(tid, sizeof(tid), "AthenaSeason:athenaseason%d", vg);
                    } else {
                        snprintf(key, sizeof(key), "AthenaSeason:%s", season_passes[i]);
                        snprintf(tid, sizeof(tid), "AthenaSeason:%s", season_passes[i]);
                    }
                    if (items && !cJSON_GetObjectItem(items, key)) {
                        cJSON *sp = cJSON_CreateObject();
                        cJSON_AddStringToObject(sp, "templateId", tid);
                        cJSON *sp_attrs = cJSON_CreateObject();
                        cJSON_AddBoolToObject  (sp_attrs, "purchased", 0);
                        cJSON_AddNumberToObject(sp_attrs, "currency_season_total", 1);
                        cJSON_AddBoolToObject  (sp_attrs, "tier_one_rewards_given", 0);
                        cJSON_AddBoolToObject  (sp_attrs, "purchased_at_least_once", 0);
                        cJSON_AddNumberToObject(sp_attrs, "level", 1);
                        cJSON_AddStringToObject(sp_attrs, "purchase_date", "0001-01-01T00:00:00.000Z");
                        cJSON_AddStringToObject(sp_attrs, "purchase_context", "None");
                        cJSON_AddItemToObject  (sp_attrs, "purchased_offers", cJSON_CreateArray());
                        cJSON_AddItemToObject(sp, "attributes", sp_attrs);
                        cJSON_AddNumberToObject(sp, "quantity", 1);
                        profile_add_item(profile, key, sp, NULL);
                        cJSON_Delete(sp);
                    }
                }
            }

            /* ── Modular locker loadouts for Ch5+ (v28+) ───────── */
            if (version_f >= 28.00f) {
                cJSON *items = cJSON_GetObjectItem(profile, "items");
                struct { const char *key; const char *tid; const char *slots_json; } loadouts[] = {
                    { "CYBERCHARACTER", "CosmeticLoadout:LoadoutSchema_Character",
                      "[{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Character\","
                      "\"equipped_item\":\"AthenaCharacter:cid_001_athena_commando_f_default\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Backpack\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Pickaxe\","
                      "\"equipped_item\":\"AthenaPickaxe:defaultpickaxe\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Glider\","
                      "\"equipped_item\":\"AthenaGlider:defaultglider\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Contrails\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Aura\"}]" },
                    { "CYBEREMOTES", "CosmeticLoadout:LoadoutSchema_Emotes",
                      "[{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Emote_0\","
                      "\"equipped_item\":\"AthenaDance:eid_boogiedown\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Emote_1\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Emote_2\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Emote_3\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Emote_4\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Emote_5\"}]" },
                    { "CYBERJAM", "CosmeticLoadout:LoadoutSchema_Jam",
                      "[{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_JamSong0\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_JamSong1\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_JamSong2\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_JamSong3\"}]" },
                    { "CYBERSPARKS", "CosmeticLoadout:LoadoutSchema_Sparks",
                      "[{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Bass\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Guitar\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Drum\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Keyboard\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Microphone\"}]" },
                    { "CYBERVEHICLE_SUV", "CosmeticLoadout:LoadoutSchema_Vehicle_SUV",
                      "[{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Vehicle_Body_SUV\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Vehicle_Skin_SUV\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Vehicle_Wheel_SUV\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Vehicle_DriftSmoke_SUV\"},"
                      "{\"slot_template\":\"CosmeticLoadoutSlotTemplate:LoadoutSlot_Vehicle_Booster_SUV\"}]" },
                    { NULL, NULL, NULL }
                };
                for (int i = 0; loadouts[i].key; i++) {
                    if (items && !cJSON_GetObjectItem(items, loadouts[i].key)) {
                        cJSON *lo = cJSON_CreateObject();
                        cJSON_AddStringToObject(lo, "templateId", loadouts[i].tid);
                        cJSON *lo_attrs = cJSON_CreateObject();
                        cJSON *slots = cJSON_Parse(loadouts[i].slots_json);
                        cJSON_AddItemToObject(lo_attrs, "slots", slots ? slots : cJSON_CreateArray());
                        cJSON_AddItemToObject(lo, "attributes", lo_attrs);
                        cJSON_AddNumberToObject(lo, "quantity", 1);
                        profile_add_item(profile, loadouts[i].key, lo, NULL);
                        cJSON_Delete(lo);
                    }
                }

                /* Update loadout_presets to include new loadouts */
                cJSON *presets = cJSON_GetObjectItem(attrs, "loadout_presets");
                if (presets) {
                    struct { const char *tid; const char *key; } preset_map[] = {
                        { "CosmeticLoadout:LoadoutSchema_Character", "CYBERCHARACTER" },
                        { "CosmeticLoadout:LoadoutSchema_Emotes", "CYBEREMOTES" },
                        { "CosmeticLoadout:LoadoutSchema_Jam", "CYBERJAM" },
                        { "CosmeticLoadout:LoadoutSchema_Sparks", "CYBERSPARKS" },
                        { "CosmeticLoadout:LoadoutSchema_Vehicle_SUV", "CYBERVEHICLE_SUV" },
                        { NULL, NULL }
                    };
                    for (int i = 0; preset_map[i].tid; i++) {
                        if (!cJSON_GetObjectItem(presets, preset_map[i].tid)) {
                            cJSON *pm = cJSON_CreateObject();
                            cJSON_AddStringToObject(pm, "0", preset_map[i].key);
                            cJSON_AddItemToObject(presets, preset_map[i].tid, pm);
                        }
                    }
                }
            }

            profile_bump_rvn(profile);
            profile_save(account_id, profile_id, profile);
        }

        cJSON *fu = full_update(profile);
        send_mcp_response(c, profile_id, profile, fu, NULL, NULL);
        cJSON_Delete(fu);
        goto cleanup;
    }

    /* ---- EquipBattleRoyaleCustomization --------------------------- */
    if (strcmp(command, "EquipBattleRoyaleCustomization") == 0) {
        cJSON *slot_name    = cJSON_GetObjectItem(body, "slotName");
        cJSON *item_to_slot = cJSON_GetObjectItem(body, "itemToSlot");
        cJSON *slot_index   = cJSON_GetObjectItem(body, "indexWithinSlot");
        cJSON *variant_upd  = cJSON_GetObjectItem(body, "variantUpdates");

        if (cJSON_IsString(slot_name) && cJSON_IsString(item_to_slot)) {
            const char *slot = slot_name->valuestring;
            const char *item = item_to_slot->valuestring;

            /* Map slot name to stat name */
            const char *stat_name = NULL;
            if      (strcmp(slot, "Character")     == 0) stat_name = "favorite_character";
            else if (strcmp(slot, "Backpack")       == 0) stat_name = "favorite_backpack";
            else if (strcmp(slot, "Pickaxe")        == 0) stat_name = "favorite_pickaxe";
            else if (strcmp(slot, "Glider")         == 0) stat_name = "favorite_glider";
            else if (strcmp(slot, "SkyDiveContrail")== 0) stat_name = "favorite_skydivecontrail";
            else if (strcmp(slot, "MusicPack")      == 0) stat_name = "favorite_musicpack";
            else if (strcmp(slot, "LoadingScreen")  == 0) stat_name = "favorite_loadingscreen";

            if (stat_name) {
                cJSON *val = cJSON_CreateString(item);
                profile_modify_stat(profile, stat_name, val, changes);
                cJSON_Delete(val);
            } else if (strcmp(slot, "Dance") == 0 || strcmp(slot, "ItemWrap") == 0) {
                int is_dance = (strcmp(slot, "Dance") == 0);
                const char *arr_stat = is_dance ? "favorite_dance" : "favorite_itemwraps";
                int capacity = is_dance ? 6 : 7;
                int idx = slot_index ? slot_index->valueint : 0;

                cJSON *stats = cJSON_GetObjectItem(profile, "stats");
                cJSON *attrs = stats ? cJSON_GetObjectItem(stats, "attributes") : NULL;
                cJSON *arr   = attrs ? cJSON_GetObjectItem(attrs, arr_stat)     : NULL;

                if (!arr) { arr = cJSON_CreateArray(); }
                else { arr = cJSON_Duplicate(arr, 1); }

                /* Fill up to capacity */
                while (cJSON_GetArraySize(arr) < capacity)
                    cJSON_AddItemToArray(arr, cJSON_CreateString(""));

                if (idx == -1) {
                    for (int i = 0; i < capacity; i++) {
                        cJSON *it = cJSON_GetArrayItem(arr, i);
                        if (it) cJSON_SetValuestring(it, item);
                    }
                } else if (idx < capacity) {
                    cJSON *it = cJSON_GetArrayItem(arr, idx);
                    if (it) cJSON_SetValuestring(it, item);
                }

                profile_modify_stat(profile, arr_stat, arr, changes);
                cJSON_Delete(arr);
            }

            /* Apply variant updates if any */
            if (cJSON_IsArray(variant_upd) && cJSON_GetArraySize(variant_upd) > 0) {
                cJSON *items = cJSON_GetObjectItem(profile, "items");
                cJSON *slot_item = items ? cJSON_GetObjectItem(items, item) : NULL;
                if (slot_item) {
                    cJSON *item_attrs    = cJSON_GetObjectItem(slot_item, "attributes");
                    cJSON *variants      = item_attrs ? cJSON_GetObjectItem(item_attrs, "variants") : NULL;
                    if (variants) {
                        cJSON *vu;
                        cJSON_ArrayForEach(vu, variant_upd) {
                            cJSON *channel = cJSON_GetObjectItem(vu, "channel");
                            cJSON *active  = cJSON_GetObjectItem(vu, "active");
                            if (!cJSON_IsString(channel)) continue;
                            cJSON *v;
                            cJSON_ArrayForEach(v, variants) {
                                cJSON *vc = cJSON_GetObjectItem(v, "channel");
                                if (cJSON_IsString(vc) && strcmp(vc->valuestring, channel->valuestring) == 0) {
                                    cJSON_DeleteItemFromObject(v, "active");
                                    cJSON_AddItemToObject(v, "active", cJSON_Duplicate(active, 1));
                                }
                            }
                        }
                        cJSON *val = cJSON_Duplicate(variants, 1);
                        profile_change_item_attr(profile, item, "variants", val, changes);
                        cJSON_Delete(val);
                    }
                }
            }
        }

        profile_bump_rvn(profile);
        profile_save(account_id, profile_id, profile);
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- SetCosmeticLockerSlot ------------------------------------ */
    if (strcmp(command, "SetCosmeticLockerSlot") == 0) {
        cJSON *locker_item   = cJSON_GetObjectItem(body, "lockerItem");
        cJSON *category      = cJSON_GetObjectItem(body, "category");
        cJSON *item_to_slot  = cJSON_GetObjectItem(body, "itemToSlot");
        cJSON *slot_index    = cJSON_GetObjectItem(body, "slotIndex");
        cJSON *variant_upd   = cJSON_GetObjectItem(body, "variantUpdates");

        if (cJSON_IsString(locker_item) && cJSON_IsString(category)) {
            cJSON *items      = cJSON_GetObjectItem(profile, "items");
            cJSON *locker_obj = items ? cJSON_GetObjectItem(items, locker_item->valuestring) : NULL;

            if (locker_obj) {
                cJSON *attrs       = cJSON_GetObjectItem(locker_obj, "attributes");
                cJSON *slot_data   = attrs ? cJSON_GetObjectItem(attrs, "locker_slots_data") : NULL;
                cJSON *slots_obj   = slot_data ? cJSON_GetObjectItem(slot_data, "slots") : NULL;
                cJSON *cat_slot    = slots_obj ? cJSON_GetObjectItem(slots_obj, category->valuestring) : NULL;

                if (!cat_slot && slots_obj) {
                    cat_slot = cJSON_CreateObject();
                    cJSON_AddItemToObject(cat_slot, "items",          cJSON_CreateArray());
                    cJSON_AddItemToObject(cat_slot, "activeVariants", cJSON_CreateArray());
                    cJSON_AddItemToObject(slots_obj, category->valuestring, cat_slot);
                }

                if (cat_slot) {
                    cJSON *items_arr = cJSON_GetObjectItem(cat_slot, "items");
                    int idx = slot_index ? slot_index->valueint : 0;
                    if (idx < 0) idx = 0;

                    /* Grow the array if needed */
                    while (cJSON_GetArraySize(items_arr) <= idx)
                        cJSON_AddItemToArray(items_arr, cJSON_CreateString(""));

                    cJSON *slot_entry = cJSON_GetArrayItem(items_arr, idx);
                    if (slot_entry && cJSON_IsString(item_to_slot))
                        cJSON_SetValuestring(slot_entry, item_to_slot->valuestring);

                    /* Variant updates */
                    if (cJSON_IsArray(variant_upd) && cJSON_GetArraySize(variant_upd) > 0) {
                        cJSON *active_v = cJSON_CreateArray();
                        cJSON *vobj = cJSON_CreateObject();
                        cJSON_AddItemToObject(vobj, "variants", cJSON_Duplicate(variant_upd, 1));
                        cJSON_AddItemToArray(active_v, vobj);
                        cJSON_DeleteItemFromObject(cat_slot, "activeVariants");
                        cJSON_AddItemToObject(cat_slot, "activeVariants", active_v);
                    }

                    cJSON *new_slot_data = cJSON_Duplicate(slot_data, 1);
                    profile_change_item_attr(profile, locker_item->valuestring,
                                            "locker_slots_data", new_slot_data, changes);
                    cJSON_Delete(new_slot_data);
                }
            }
        }

        profile_bump_rvn(profile);
        profile_save(account_id, profile_id, profile);
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- SetCosmeticLockerBanner ---------------------------------- */
    if (strcmp(command, "SetCosmeticLockerBanner") == 0) {
        cJSON *locker_item  = cJSON_GetObjectItem(body, "lockerItem");
        cJSON *banner_icon  = cJSON_GetObjectItem(body, "bannerIconTemplateName");
        cJSON *banner_color = cJSON_GetObjectItem(body, "bannerColorTemplateName");

        if (cJSON_IsString(locker_item)) {
            if (cJSON_IsString(banner_icon)) {
                cJSON *val = cJSON_CreateString(banner_icon->valuestring);
                profile_change_item_attr(profile, locker_item->valuestring,
                                         "banner_icon_template", val, changes);
                cJSON_Delete(val);
            }
            if (cJSON_IsString(banner_color)) {
                cJSON *val = cJSON_CreateString(banner_color->valuestring);
                profile_change_item_attr(profile, locker_item->valuestring,
                                         "banner_color_template", val, changes);
                cJSON_Delete(val);
            }
        }

        profile_bump_rvn(profile);
        profile_save(account_id, profile_id, profile);
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- SetCosmeticLockerName ------------------------------------ */
    if (strcmp(command, "SetCosmeticLockerName") == 0) {
        cJSON *locker_item = cJSON_GetObjectItem(body, "lockerItem");
        cJSON *name        = cJSON_GetObjectItem(body, "name");
        if (cJSON_IsString(locker_item) && cJSON_IsString(name)) {
            profile_change_item_attr(profile, locker_item->valuestring,
                                     "locker_name", name, changes);
        }
        profile_bump_rvn(profile);
        profile_save(account_id, profile_id, profile);
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- CopyCosmeticLoadout ------------------------------------- */
    if (strcmp(command, "CopyCosmeticLoadout") == 0) {
        profile_bump_rvn(profile);
        profile_save(account_id, profile_id, profile);
        cJSON *fu = full_update(profile);
        send_mcp_response(c, profile_id, profile, fu, NULL, NULL);
        cJSON_Delete(fu);
        goto cleanup;
    }

    /* ---- SetItemFavoriteStatus ------------------------------------ */
    if (strcmp(command, "SetItemFavoriteStatus") == 0) {
        cJSON *target = cJSON_GetObjectItem(body, "targetItemId");
        cJSON *fav    = cJSON_GetObjectItem(body, "bFavorite");
        if (cJSON_IsString(target) && cJSON_IsBool(fav)) {
            cJSON *val = cJSON_Duplicate(fav, 1);
            profile_change_item_attr(profile, target->valuestring, "favorite", val, changes);
            cJSON_Delete(val);
            profile_save(account_id, profile_id, profile);
        }
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- SetItemFavoriteStatusBatch ------------------------------ */
    if (strcmp(command, "SetItemFavoriteStatusBatch") == 0) {
        cJSON *item_ids  = cJSON_GetObjectItem(body, "itemIds");
        cJSON *fav_stats = cJSON_GetObjectItem(body, "itemFavStatus");
        if (cJSON_IsArray(item_ids) && cJSON_IsArray(fav_stats)) {
            int count = cJSON_GetArraySize(item_ids);
            for (int i = 0; i < count; i++) {
                cJSON *id  = cJSON_GetArrayItem(item_ids,  i);
                cJSON *fav = cJSON_GetArrayItem(fav_stats, i);
                if (cJSON_IsString(id)) {
                    cJSON *val = cJSON_Duplicate(fav, 1);
                    profile_change_item_attr(profile, id->valuestring, "favorite", val, changes);
                    cJSON_Delete(val);
                }
            }
            profile_bump_rvn(profile);
            profile_save(account_id, profile_id, profile);
        }
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- MarkItemSeen -------------------------------------------- */
    if (strcmp(command, "MarkItemSeen") == 0) {
        cJSON *item_ids = cJSON_GetObjectItem(body, "itemIds");
        if (cJSON_IsArray(item_ids)) {
            cJSON *id;
            cJSON_ArrayForEach(id, item_ids) {
                if (cJSON_IsString(id)) {
                    cJSON *t = cJSON_CreateBool(1);
                    profile_change_item_attr(profile, id->valuestring, "item_seen", t, changes);
                    cJSON_Delete(t);
                }
            }
        }
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- SetItemArchivedStatusBatch ------------------------------ */
    if (strcmp(command, "SetItemArchivedStatusBatch") == 0) {
        cJSON *item_ids = cJSON_GetObjectItem(body, "itemIds");
        cJSON *archived = cJSON_GetObjectItem(body, "archived");
        if (cJSON_IsArray(item_ids) && cJSON_IsBool(archived)) {
            cJSON *id;
            cJSON_ArrayForEach(id, item_ids) {
                if (cJSON_IsString(id)) {
                    cJSON *val = cJSON_Duplicate(archived, 1);
                    profile_change_item_attr(profile, id->valuestring, "archived", val, changes);
                    cJSON_Delete(val);
                }
            }
        }
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- RemoveGiftBox ------------------------------------------- */
    if (strcmp(command, "RemoveGiftBox") == 0) {
        cJSON *single = cJSON_GetObjectItem(body, "giftBoxItemId");
        cJSON *multi  = cJSON_GetObjectItem(body, "giftBoxItemIds");
        if (cJSON_IsString(single))
            profile_remove_item(profile, single->valuestring, changes);
        if (cJSON_IsArray(multi)) {
            cJSON *id;
            cJSON_ArrayForEach(id, multi)
                if (cJSON_IsString(id))
                    profile_remove_item(profile, id->valuestring, changes);
        }
        profile_bump_rvn(profile);
        profile_save(account_id, profile_id, profile);
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- SetAffiliateName ---------------------------------------- */
    if (strcmp(command, "SetAffiliateName") == 0) {
        cJSON *name = cJSON_GetObjectItem(body, "affiliateName");
        if (cJSON_IsString(name)) {
            cJSON *common = get_or_create(account_id, "common_core");
            if (common) {
                cJSON *val = cJSON_CreateString(name->valuestring);
                profile_modify_stat(common, "mtx_affiliate", val, changes);
                cJSON_Delete(val);
                char ts[64]; now_iso(ts, sizeof(ts));
                cJSON *ts_val = cJSON_CreateString(ts);
                profile_modify_stat(common, "mtx_affiliate_set_time", ts_val, NULL);
                cJSON_Delete(ts_val);
                profile_bump_rvn(common);
                profile_save(account_id, "common_core", common);
                cJSON_Delete(common);
            }
        }
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- PurchaseCatalogEntry ------------------------------------ */
    if (strcmp(command, "PurchaseCatalogEntry") == 0) {
        cJSON *offer_id = cJSON_GetObjectItem(body, "offerId");
        if (!cJSON_IsString(offer_id)) {
            cyber_send_error(c, 400, ERR_CATALOG_OUTDATED, "offerId missing.", 28001);
            goto cleanup;
        }

        /* Load the shop catalog */
        char *raw = NULL;
        FILE *f = fopen("responses/catalog/shop.json", "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
            raw = malloc(sz + 1); fread(raw, 1, sz, f); raw[sz] = '\0'; fclose(f);
        }
        cJSON *shop = raw ? cJSON_Parse(raw) : NULL;
        if (raw) free(raw);

        cJSON *entry = NULL;
        if (shop) {
            cJSON *storefronts = cJSON_GetObjectItem(shop, "storefronts");
            cJSON *sf;
            cJSON_ArrayForEach(sf, storefronts) {
                cJSON *catalog_entries = cJSON_GetObjectItem(sf, "catalogEntries");
                cJSON *ce;
                cJSON_ArrayForEach(ce, catalog_entries) {
                    cJSON *oid = cJSON_GetObjectItem(ce, "offerId");
                    if (cJSON_IsString(oid) &&
                        strcmp(oid->valuestring, offer_id->valuestring) == 0) {
                        entry = ce;
                        break;
                    }
                }
                if (entry) break;
            }
        }

        if (!entry) {
            cyber_send_error(c, 400, ERR_CATALOG_OUTDATED, "Offer not found.", 28001);
            if (shop) cJSON_Delete(shop);
            goto cleanup;
        }

        cJSON *loot_result = cJSON_CreateArray();
        cJSON *grants = cJSON_GetObjectItem(entry, "itemGrants");
        if (cJSON_IsArray(grants)) {
            cJSON *grant;
            cJSON_ArrayForEach(grant, grants) {
                cJSON *tid = cJSON_GetObjectItem(grant, "templateId");
                if (!cJSON_IsString(tid)) continue;

                char ts[64]; now_iso(ts, sizeof(ts));
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "templateId", tid->valuestring);
                cJSON *attrs = cJSON_CreateObject();
                cJSON_AddNumberToObject(attrs, "max_level_bonus", 0);
                cJSON_AddNumberToObject(attrs, "level",           1);
                cJSON_AddBoolToObject  (attrs, "item_seen",       0);
                cJSON_AddNumberToObject(attrs, "xp",              0);
                cJSON_AddItemToObject  (attrs, "variants",        cJSON_CreateArray());
                cJSON_AddStringToObject(attrs, "creation_time",   ts);
                cJSON_AddBoolToObject  (attrs, "favorite",        0);
                cJSON_AddItemToObject  (item, "attributes", attrs);
                cJSON_AddNumberToObject(item, "quantity", 1);

                profile_add_item(athena, tid->valuestring, item, changes);
                cJSON_Delete(item);

                cJSON *loot = cJSON_CreateObject();
                cJSON_AddStringToObject(loot, "itemType",    tid->valuestring);
                cJSON_AddStringToObject(loot, "itemGuid",    tid->valuestring);
                cJSON_AddStringToObject(loot, "itemProfile", "athena");
                cJSON_AddNumberToObject(loot, "quantity",    1);
                cJSON_AddItemToArray(loot_result, loot);
            }
        }

        profile_bump_rvn(athena);
        profile_save(account_id, "athena", athena);

        /* notifications */
        notifications = cJSON_CreateArray();
        cJSON *notif = cJSON_CreateObject();
        cJSON_AddStringToObject(notif, "type",    "CatalogPurchase");
        cJSON_AddBoolToObject  (notif, "primary", 1);
        cJSON *lr_wrap = cJSON_CreateObject();
        cJSON_AddItemToObject(lr_wrap, "items", loot_result);
        cJSON_AddItemToObject(notif, "lootResult", lr_wrap);
        cJSON_AddItemToArray(notifications, notif);

        /* multiUpdate for athena */
        multi_update = cJSON_CreateArray();
        cJSON *athena_upd = cJSON_CreateObject();
        cJSON *athena_rvn = cJSON_GetObjectItem(athena, "rvn");
        cJSON *athena_cr  = cJSON_GetObjectItem(athena, "commandRevision");
        cJSON_AddNumberToObject(athena_upd, "profileRevision",            athena_rvn ? athena_rvn->valueint : 1);
        cJSON_AddStringToObject(athena_upd, "profileId",                  "athena");
        cJSON_AddNumberToObject(athena_upd, "profileChangesBaseRevision", athena_rvn ? athena_rvn->valueint : 1);
        cJSON_AddItemToObject  (athena_upd, "profileChanges",             cJSON_Duplicate(changes, 1));
        cJSON_AddNumberToObject(athena_upd, "profileCommandRevision",     athena_cr  ? athena_cr->valueint  : 1);
        cJSON_AddItemToArray(multi_update, athena_upd);

        if (shop) cJSON_Delete(shop);
        send_mcp_response(c, profile_id, profile, cJSON_CreateArray(), multi_update, notifications);
        goto cleanup;
    }

    /* ---- ClaimMfaEnabled ----------------------------------------- */
    if (strcmp(command, "ClaimMfaEnabled") == 0) {
        cJSON *val = cJSON_CreateBool(1);
        profile_modify_stat(profile, "mfa_reward_claimed", val, NULL);
        cJSON_Delete(val);
        profile_save(account_id, profile_id, profile);
        cJSON *fu = full_update(profile);
        send_mcp_response(c, profile_id, profile, fu, NULL, NULL);
        cJSON_Delete(fu);
        goto cleanup;
    }

    /* ---- SetMtxPlatform ------------------------------------------ */
    if (strcmp(command, "SetMtxPlatform") == 0) {
        cJSON *platform = cJSON_GetObjectItem(body, "newPlatform");
        if (cJSON_IsString(platform)) {
            profile_modify_stat(profile, "current_mtx_platform", platform, changes);
        }
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- SetReceiveGiftsEnabled ---------------------------------- */
    if (strcmp(command, "SetReceiveGiftsEnabled") == 0) {
        cJSON *enabled = cJSON_GetObjectItem(body, "bReceiveGifts");
        if (cJSON_IsBool(enabled)) {
            profile_modify_stat(profile, "allowed_to_receive_gifts", enabled, changes);
        }
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- AthenaPinQuest ------------------------------------------ */
    if (strcmp(command, "AthenaPinQuest") == 0) {
        cJSON *quest = cJSON_GetObjectItem(body, "pinnedQuest");
        if (cJSON_IsString(quest)) {
            profile_modify_stat(profile, "pinned_quest", quest, changes);
            profile_bump_rvn(profile);
            profile_save(account_id, profile_id, profile);
        }
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- RequestRestedStateIncrease ------------------------------ */
    if (strcmp(command, "RequestRestedStateIncrease") == 0) {
        cJSON *xp_acc = cJSON_GetObjectItem(body, "restedXpGenAccumulated");
        if (cJSON_IsNumber(xp_acc)) {
            cJSON *stats = cJSON_GetObjectItem(profile, "stats");
            cJSON *attrs = stats ? cJSON_GetObjectItem(stats, "attributes") : NULL;
            cJSON *cur   = attrs ? cJSON_GetObjectItem(attrs, "book_xp")    : NULL;
            double new_xp = (cur ? cur->valuedouble : 0) + xp_acc->valuedouble;
            cJSON *val = cJSON_CreateNumber(new_xp);
            profile_modify_stat(profile, "book_xp", val, changes);
            cJSON_Delete(val);
            profile_bump_rvn(profile);
            profile_save(account_id, profile_id, profile);
        }
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- RedeemRealMoneyPurchases -------------------------------- */
    if (strcmp(command, "RedeemRealMoneyPurchases") == 0) {
        cJSON *val = cJSON_CreateObject();
        cJSON_AddItemToObject  (val, "receipts",        cJSON_CreateArray());
        cJSON_AddItemToObject  (val, "ignoredReceipts", cJSON_CreateArray());
        cJSON_AddNumberToObject(val, "version",         1);
        profile_modify_stat(profile, "in_app_purchases", val, changes);
        cJSON_Delete(val);
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- RefundMtxPurchase --------------------------------------- */
    if (strcmp(command, "RefundMtxPurchase") == 0) {
        char ts[64]; now_iso(ts, sizeof(ts));
        char refund_id[64];
        snprintf(refund_id, sizeof(refund_id), "refund-%ld", (long)time(NULL));

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "templateId", "Currency:MtxComplimentary");
        cJSON *attrs = cJSON_CreateObject();
        cJSON_AddStringToObject(attrs, "platform", "Shared");
        cJSON_AddItemToObject(item, "attributes", attrs);
        cJSON_AddNumberToObject(item, "quantity", 1500);

        profile_add_item(profile, refund_id, item, changes);
        cJSON_Delete(item);
        profile_bump_rvn(profile);
        profile_save(account_id, profile_id, profile);
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- DeleteCosmeticLoadout ----------------------------------- */
    if (strcmp(command, "DeleteCosmeticLoadout") == 0) {
        cJSON *index_j = cJSON_GetObjectItem(body, "index");
        if (cJSON_IsNumber(index_j)) {
            cJSON *stats = cJSON_GetObjectItem(profile, "stats");
            cJSON *attrs = stats ? cJSON_GetObjectItem(stats, "attributes") : NULL;
            cJSON *loadouts = attrs ? cJSON_GetObjectItem(attrs, "loadouts") : NULL;
            if (cJSON_IsArray(loadouts)) {
                int idx = index_j->valueint;
                cJSON *slot = cJSON_GetArrayItem(loadouts, idx);
                if (slot) cJSON_SetValuestring(slot, "");
            }
        }
        profile_bump_rvn(profile);
        profile_save(account_id, profile_id, profile);
        cJSON *fu = full_update(profile);
        send_mcp_response(c, profile_id, profile, fu, NULL, NULL);
        cJSON_Delete(fu);
        goto cleanup;
    }

    /* ---- PutModularCosmeticLoadout (Ch5+ modular loadouts) ------- */
    if (strcmp(command, "PutModularCosmeticLoadout") == 0) {
        /* Body: { loadoutData: "<JSON string>", loadoutType: "...", presetId: 0|"uuid" } */
        cJSON *loadout_data_str = cJSON_GetObjectItem(body, "loadoutData");
        cJSON *loadout_type     = cJSON_GetObjectItem(body, "loadoutType");
        cJSON *preset_id_j      = cJSON_GetObjectItem(body, "presetId");

        if (cJSON_IsString(loadout_data_str) && cJSON_IsString(loadout_type)) {
            cJSON *loadout_data = cJSON_Parse(loadout_data_str->valuestring);
            if (!loadout_data) loadout_data = cJSON_CreateObject();

            const char *lt = loadout_type->valuestring;

            /* loadout_presets stat is a map: loadoutType -> { presetId -> itemId } */
            cJSON *stats = cJSON_GetObjectItem(profile, "stats");
            cJSON *attrs = stats ? cJSON_GetObjectItem(stats, "attributes") : NULL;
            cJSON *lp    = attrs ? cJSON_GetObjectItem(attrs, "loadout_presets") : NULL;

            if (!lp && attrs) {
                lp = cJSON_CreateObject();
                cJSON_AddItemToObject(attrs, "loadout_presets", lp);
            }

            cJSON *lt_map = lp ? cJSON_GetObjectItem(lp, lt) : NULL;
            if (!lt_map && lp) {
                lt_map = cJSON_CreateObject();
                cJSON_AddItemToObject(lp, lt, lt_map);
            }

            /* Determine the string key for this preset */
            char preset_key[64] = "0";
            if (cJSON_IsNumber(preset_id_j))
                snprintf(preset_key, sizeof(preset_key), "%d", preset_id_j->valueint);
            else if (cJSON_IsString(preset_id_j))
                snprintf(preset_key, sizeof(preset_key), "%s", preset_id_j->valuestring);

            cJSON *existing_item_id = lt_map ? cJSON_GetObjectItem(lt_map, preset_key) : NULL;

            if (existing_item_id && cJSON_IsString(existing_item_id)) {
                /* Update existing loadout item */
                const char *item_id = existing_item_id->valuestring;
                cJSON *items = cJSON_GetObjectItem(profile, "items");
                cJSON *lo    = items ? cJSON_GetObjectItem(items, item_id) : NULL;
                if (lo) {
                    cJSON *slots_new = cJSON_GetObjectItem(loadout_data, "slots");
                    if (slots_new) {
                        cJSON *val = cJSON_Duplicate(slots_new, 1);
                        profile_change_item_attr(profile, item_id, "slots", val, changes);
                        cJSON_Delete(val);
                    }
                    cJSON *dn = cJSON_GetObjectItem(loadout_data, "display_name");
                    if (dn) {
                        cJSON *val = cJSON_Duplicate(dn, 1);
                        profile_change_item_attr(profile, item_id, "display_name", val, changes);
                        cJSON_Delete(val);
                    }
                }
            } else {
                /* Create new loadout item */
                char new_item_id[64];
                snprintf(new_item_id, sizeof(new_item_id), "cyber_%s_%s",
                         preset_key, lt + (strlen(lt) > 16 ? strlen(lt) - 16 : 0));

                cJSON *new_item = cJSON_CreateObject();
                cJSON_AddStringToObject(new_item, "templateId", lt);
                cJSON *new_attrs = cJSON_Duplicate(loadout_data, 1);
                if (!new_attrs) new_attrs = cJSON_CreateObject();
                cJSON_AddItemToObject(new_item, "attributes", new_attrs);
                cJSON_AddNumberToObject(new_item, "quantity", 1);

                profile_add_item(profile, new_item_id, new_item, changes);
                cJSON_Delete(new_item);

                /* Register in loadout_presets map */
                if (lt_map) {
                    cJSON *id_val = cJSON_CreateString(new_item_id);
                    cJSON_DeleteItemFromObject(lt_map, preset_key);
                    cJSON_AddItemToObject(lt_map, preset_key, id_val);
                }

                /* Update the loadout_presets stat change */
                if (lp) {
                    cJSON *lp_dup = cJSON_Duplicate(lp, 1);
                    profile_modify_stat(profile, "loadout_presets", lp_dup, changes);
                    cJSON_Delete(lp_dup);
                }
                /* Signal commit */
                cJSON *commit_val = cJSON_CreateString("COMMITTED");
                profile_modify_stat(profile, "locker_two_phase_commit", commit_val, changes);
                cJSON_Delete(commit_val);
            }

            cJSON_Delete(loadout_data);
        }

        profile_bump_rvn(profile);
        profile_save(account_id, profile_id, profile);
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- SetFactionChoice ---------------------------------------- */
    if (strcmp(command, "SetFactionChoice") == 0) {
        cJSON *token_tid = cJSON_GetObjectItem(body, "factionTokenTemplateId");
        if (cJSON_IsString(token_tid)) {
            char ts[64]; now_iso(ts, sizeof(ts));
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "templateId", token_tid->valuestring);
            cJSON *fa = cJSON_CreateObject();
            cJSON_AddNumberToObject(fa, "level", 1);
            cJSON_AddItemToObject(item, "attributes", fa);
            cJSON_AddNumberToObject(item, "quantity", 1);
            profile_add_item(athena, token_tid->valuestring, item, changes);
            cJSON_Delete(item);
            profile_bump_rvn(athena);
            profile_save(account_id, "athena", athena);
        }
        send_mcp_response(c, profile_id, profile, changes, NULL, NULL);
        goto cleanup;
    }

    /* ---- ExchangeGameCurrencyForBattlePassOffer / catch-alls ----- */
    if (strcmp(command, "GetMcpTimeForLogin")                    == 0 ||
        strcmp(command, "ClientQuestLogin")                       == 0 ||
        strcmp(command, "RefreshExpeditions")                     == 0 ||
        strcmp(command, "SetForcedIntroPlayed")                   == 0 ||
        strcmp(command, "PopulatePrerolledOffers")                == 0 ||
        strcmp(command, "BulkEquipBattleRoyaleCustomization")     == 0 ||
        strcmp(command, "SetRandomCosmeticLoadoutFlag")           == 0 ||
        strcmp(command, "SetLoadoutShuffleEnabled")               == 0 ||
        strcmp(command, "MarkNewQuestNotificationSent")           == 0 ||
        strcmp(command, "SetHardcoreModifier")                    == 0 ||
        strcmp(command, "IncrementNamedCounterStat")              == 0 ||
        strcmp(command, "QuestLogin")                             == 0 ||
        strcmp(command, "SetCosmeticLockerSlots")                 == 0 ||
        strcmp(command, "ExchangeGameCurrencyForBattlePassOffer") == 0 ||
        strcmp(command, "ExchangeGameCurrencyForSeasonPassOffer") == 0 ||
        strcmp(command, "RedeemSTWAccoladeTokens")                == 0) {
        cJSON *fu = full_update(profile);
        send_mcp_response(c, profile_id, profile, fu, NULL, NULL);
        cJSON_Delete(fu);
        goto cleanup;
    }

    /* ---- Unknown command ----------------------------------------- */
    cyber_warn("Unknown MCP command: %s", command);
    cyber_send_error(c, 404, ERR_OP_NOT_FOUND, "Operation not found.", 16035);

cleanup:
    cJSON_Delete(body);
    cJSON_Delete(changes);
    if (multi_update)  cJSON_Delete(multi_update);
    if (notifications) cJSON_Delete(notifications);
    if (athena)        cJSON_Delete(athena);
    if (profile)       cJSON_Delete(profile);
}
