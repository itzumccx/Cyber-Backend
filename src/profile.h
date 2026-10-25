#ifndef CYBER_PROFILE_H
#define CYBER_PROFILE_H

#include "../../vendor/cjson/cJSON.h"

/* Read a profile JSON from disk.
   Returns a heap-allocated cJSON object, caller must cJSON_Delete() it.
   Returns NULL if the file does not exist. */
cJSON *profile_read(const char *account_id, const char *profile_id);

/* Read a profile template from profile_template/profiles/.
   Returns a heap-allocated cJSON object, caller must cJSON_Delete() it.
   Returns NULL if the template does not exist. */
cJSON *profile_read_template(const char *profile_id);

/* Write a profile JSON to disk. Returns 1 on success, 0 on failure. */
int profile_save(const char *account_id, const char *profile_id, cJSON *profile);

/* Read a locker JSON (lockerv3.json or lockerv4.json). */
cJSON *profile_read_locker(const char *account_id, int version);

/* Read a locker template. */
cJSON *profile_read_locker_template(int version);

/* Write a locker JSON to disk. */
int profile_save_locker(const char *account_id, int version, cJSON *locker);

/* Increment rvn and commandRevision, update the "updated" timestamp. */
void profile_bump_rvn(cJSON *profile);

/* Add an item to profile->items. Returns 1 if added, 0 if already existed. */
int profile_add_item(cJSON *profile, const char *item_id, cJSON *item,
                     cJSON *changes_arr);

/* Remove an item from profile->items. Returns 1 if removed, 0 if not found. */
int profile_remove_item(cJSON *profile, const char *item_id,
                        cJSON *changes_arr);

/* Change a single attribute value on an existing item.
   Returns 1 on success, 0 if item not found. */
int profile_change_item_attr(cJSON *profile, const char *item_id,
                             const char *attr_name, cJSON *attr_value,
                             cJSON *changes_arr);

/* Modify a stat in profile->stats->attributes.
   Returns 1 on success. */
int profile_modify_stat(cJSON *profile, const char *stat_name,
                        cJSON *stat_value, cJSON *changes_arr);

/* Build the standard MCP response wrapper.
   Caller owns the returned cJSON object. */
cJSON *profile_make_response(const char *profile_id, cJSON *profile,
                             cJSON *profile_changes);

#endif
