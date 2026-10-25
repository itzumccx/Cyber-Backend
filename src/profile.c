#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_compat(path) _mkdir(path)
#else
#include <unistd.h>
#define mkdir_compat(path) mkdir(path, 0755)
#endif
#include "profile.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/* Read the full contents of a file into a heap buffer.
   Returns NULL on failure. Caller must free(). */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }

    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* Write a string to a file. Returns 1 on success. */
static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(data, f);
    fclose(f);
    return 1;
}

/* Create directories recursively (simple version for Windows/Linux). */
static void make_dirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char old = *p;
            *p = '\0';
            mkdir_compat(tmp);
            *p = old;
        }
    }
    mkdir_compat(tmp);
}

/* Current ISO 8601 timestamp into buf (at least 32 bytes). */
static void now_iso(char *buf, size_t len) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S.000Z", tm);
}

/* ------------------------------------------------------------------ */
/* Path builders                                                        */
/* ------------------------------------------------------------------ */

static void profile_path(char *out, size_t len,
                         const char *account_id, const char *profile_id) {
    snprintf(out, len, "profile/%s/profiles/profile_%s.json",
             account_id, profile_id);
}

static void template_path(char *out, size_t len, const char *profile_id) {
    snprintf(out, len, "profile_template/profiles/profile_%s.json", profile_id);
}

static void locker_path(char *out, size_t len,
                        const char *account_id, int version) {
    snprintf(out, len, "profile/%s/profiles/lockerv%d.json",
             account_id, version);
}

static void locker_template_path(char *out, size_t len, int version) {
    snprintf(out, len, "profile_template/profiles/lockerv%d.json", version);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

cJSON *profile_read(const char *account_id, const char *profile_id) {
    char path[512];
    profile_path(path, sizeof(path), account_id, profile_id);
    char *raw = read_file(path);
    if (!raw) return NULL;
    cJSON *j = cJSON_Parse(raw);
    free(raw);
    return j;
}

cJSON *profile_read_template(const char *profile_id) {
    char path[512];
    template_path(path, sizeof(path), profile_id);
    char *raw = read_file(path);
    if (!raw) return NULL;
    cJSON *j = cJSON_Parse(raw);
    free(raw);
    return j;
}

int profile_save(const char *account_id, const char *profile_id, cJSON *profile) {
    /* Make sure the directory exists */
    char dir[512];
    snprintf(dir, sizeof(dir), "profile/%s/profiles", account_id);
    make_dirs(dir);

    char path[512];
    profile_path(path, sizeof(path), account_id, profile_id);

    char *data = cJSON_Print(profile);
    if (!data) return 0;
    int ok = write_file(path, data);
    free(data);
    return ok;
}

cJSON *profile_read_locker(const char *account_id, int version) {
    char path[512];
    locker_path(path, sizeof(path), account_id, version);
    char *raw = read_file(path);
    if (!raw) return NULL;
    cJSON *j = cJSON_Parse(raw);
    free(raw);
    return j;
}

cJSON *profile_read_locker_template(int version) {
    char path[512];
    locker_template_path(path, sizeof(path), version);
    char *raw = read_file(path);
    if (!raw) return NULL;
    cJSON *j = cJSON_Parse(raw);
    free(raw);
    return j;
}

int profile_save_locker(const char *account_id, int version, cJSON *locker) {
    char dir[512];
    snprintf(dir, sizeof(dir), "profile/%s/profiles", account_id);
    make_dirs(dir);

    char path[512];
    locker_path(path, sizeof(path), account_id, version);

    char *data = cJSON_Print(locker);
    if (!data) return 0;
    int ok = write_file(path, data);
    free(data);
    return ok;
}

void profile_bump_rvn(cJSON *profile) {
    cJSON *rvn = cJSON_GetObjectItem(profile, "rvn");
    cJSON *cr  = cJSON_GetObjectItem(profile, "commandRevision");

    if (cJSON_IsNumber(rvn)) rvn->valuedouble += 1, rvn->valueint += 1;
    if (cJSON_IsNumber(cr))  cr->valuedouble  += 1, cr->valueint  += 1;

    char ts[64];
    now_iso(ts, sizeof(ts));

    cJSON *updated = cJSON_GetObjectItem(profile, "updated");
    if (updated) cJSON_SetValuestring(updated, ts);
    else cJSON_AddStringToObject(profile, "updated", ts);
}

int profile_add_item(cJSON *profile, const char *item_id, cJSON *item,
                     cJSON *changes_arr) {
    cJSON *items = cJSON_GetObjectItem(profile, "items");
    if (!items) return 0;

    /* Don't add if it already exists */
    if (cJSON_GetObjectItem(items, item_id)) return 0;

    cJSON_AddItemToObject(items, item_id, cJSON_Duplicate(item, 1));

    if (changes_arr) {
        cJSON *change = cJSON_CreateObject();
        cJSON_AddStringToObject(change, "changeType", "itemAdded");
        cJSON_AddStringToObject(change, "itemId", item_id);
        cJSON_AddItemToObject(change, "item", cJSON_Duplicate(item, 1));
        cJSON_AddItemToArray(changes_arr, change);
    }

    return 1;
}

int profile_remove_item(cJSON *profile, const char *item_id,
                        cJSON *changes_arr) {
    cJSON *items = cJSON_GetObjectItem(profile, "items");
    if (!items) return 0;
    if (!cJSON_GetObjectItem(items, item_id)) return 0;

    cJSON_DeleteItemFromObject(items, item_id);

    if (changes_arr) {
        cJSON *change = cJSON_CreateObject();
        cJSON_AddStringToObject(change, "changeType", "itemRemoved");
        cJSON_AddStringToObject(change, "itemId", item_id);
        cJSON_AddItemToArray(changes_arr, change);
    }

    return 1;
}

int profile_change_item_attr(cJSON *profile, const char *item_id,
                             const char *attr_name, cJSON *attr_value,
                             cJSON *changes_arr) {
    cJSON *items = cJSON_GetObjectItem(profile, "items");
    if (!items) return 0;
    cJSON *item = cJSON_GetObjectItem(items, item_id);
    if (!item) return 0;

    cJSON *attrs = cJSON_GetObjectItem(item, "attributes");
    if (!attrs) {
        attrs = cJSON_CreateObject();
        cJSON_AddItemToObject(item, "attributes", attrs);
    }

    cJSON_DeleteItemFromObject(attrs, attr_name);
    cJSON_AddItemToObject(attrs, attr_name, cJSON_Duplicate(attr_value, 1));

    if (changes_arr) {
        cJSON *change = cJSON_CreateObject();
        cJSON_AddStringToObject(change, "changeType", "itemAttrChanged");
        cJSON_AddStringToObject(change, "itemId", item_id);
        cJSON_AddStringToObject(change, "attributeName", attr_name);
        cJSON_AddItemToObject(change, "attributeValue", cJSON_Duplicate(attr_value, 1));
        cJSON_AddItemToArray(changes_arr, change);
    }

    return 1;
}

int profile_modify_stat(cJSON *profile, const char *stat_name,
                        cJSON *stat_value, cJSON *changes_arr) {
    cJSON *stats = cJSON_GetObjectItem(profile, "stats");
    if (!stats) {
        stats = cJSON_CreateObject();
        cJSON_AddItemToObject(profile, "stats", stats);
    }
    cJSON *attrs = cJSON_GetObjectItem(stats, "attributes");
    if (!attrs) {
        attrs = cJSON_CreateObject();
        cJSON_AddItemToObject(stats, "attributes", attrs);
    }

    cJSON_DeleteItemFromObject(attrs, stat_name);
    cJSON_AddItemToObject(attrs, stat_name, cJSON_Duplicate(stat_value, 1));

    if (changes_arr) {
        cJSON *change = cJSON_CreateObject();
        cJSON_AddStringToObject(change, "changeType", "statModified");
        cJSON_AddStringToObject(change, "name", stat_name);
        cJSON_AddItemToObject(change, "value", cJSON_Duplicate(stat_value, 1));
        cJSON_AddItemToArray(changes_arr, change);
    }

    return 1;
}

cJSON *profile_make_response(const char *profile_id, cJSON *profile,
                             cJSON *profile_changes) {
    cJSON *res = cJSON_CreateObject();

    cJSON *rvn = cJSON_GetObjectItem(profile, "rvn");
    cJSON *cr  = cJSON_GetObjectItem(profile, "commandRevision");
    int rvn_val = rvn ? rvn->valueint : 1;
    int cr_val  = cr  ? cr->valueint  : 1;

    cJSON_AddNumberToObject(res, "profileRevision", rvn_val);
    cJSON_AddStringToObject(res, "profileId", profile_id);
    cJSON_AddNumberToObject(res, "profileChangesBaseRevision", rvn_val);

    if (profile_changes)
        cJSON_AddItemToObject(res, "profileChanges", cJSON_Duplicate(profile_changes, 1));
    else
        cJSON_AddItemToObject(res, "profileChanges", cJSON_CreateArray());

    char ts[64];
    now_iso(ts, sizeof(ts));
    cJSON_AddStringToObject(res, "serverTime", ts);
    cJSON_AddNumberToObject(res, "profileCommandRevision", cr_val);
    cJSON_AddNumberToObject(res, "responseVersion", 1);

    return res;
}
