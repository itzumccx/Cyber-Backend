#ifndef CYBER_LOCKER_H
#define CYBER_LOCKER_H

#include "../../vendor/mongoose/mongoose.h"

/* GET  /api/locker/v3/:deploymentId/account/:accountId/items */
void locker_v3_get(struct mg_connection *c, struct mg_http_message *hm);

/* PUT  /api/locker/v3/:deploymentId/loadout/:loadoutType/account/:accountId/active-loadout */
void locker_v3_set_loadout(struct mg_connection *c, struct mg_http_message *hm);

/* PUT  /api/locker/v3/:deploymentId/loadout/:loadoutType/account/:accountId/loadout-preset/index/:presetIndex */
void locker_v3_preset(struct mg_connection *c, struct mg_http_message *hm);

/* GET  /api/locker/v4/:deploymentId/account/:accountId/items */
void locker_v4_get(struct mg_connection *c, struct mg_http_message *hm);

/* PUT  /api/locker/v4/:deploymentId/account/:accountId/active-loadout-group */
void locker_v4_set_loadout(struct mg_connection *c, struct mg_http_message *hm);

/* PUT  /api/locker/v4/:deploymentId/account/:accountId/loadout-group-preset/index/:presetIndex */
void locker_v4_group_preset(struct mg_connection *c, struct mg_http_message *hm);

/* PUT  /api/locker/v4/:deploymentId/loadout/:loadoutType/account/:accountId/loadout-preset/index/:presetIndex */
void locker_v4_preset(struct mg_connection *c, struct mg_http_message *hm);

/* POST /api/locker/v4/:deploymentId/account/:accountId/lock-in-immutable-item/:companion */
void locker_v4_immutable(struct mg_connection *c, struct mg_http_message *hm);

/* PATCH /api/locker/v4/:deploymentId/account/:accountId/companion-name */
void locker_v4_companion_name(struct mg_connection *c, struct mg_http_message *hm);

/* GET  /api/locker/v4/:deploymentId/account/:accountId/cosmetic-data */
void locker_v4_cosmetic_data(struct mg_connection *c, struct mg_http_message *hm);

#endif
