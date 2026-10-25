#ifndef CYBER_EOS_H
#define CYBER_EOS_H

#include "../../vendor/mongoose/mongoose.h"

/* POST /auth/v1/oauth/token  (EOS SDK v1) */
void eos_oauth_v1(struct mg_connection *c, struct mg_http_message *hm);

/* POST /epic/oauth/v2/token */
void eos_oauth_v2(struct mg_connection *c, struct mg_http_message *hm);

/* GET /epic/id/v2/sdk/accounts */
void eos_sdk_accounts(struct mg_connection *c, struct mg_http_message *hm);

/* GET /sdk/v1/default  (big EOS config blob) */
void eos_sdk_config(struct mg_connection *c, struct mg_http_message *hm);

void eos_sdk_product(struct mg_connection *c, struct mg_http_message *hm);

/* GET /epic/friends/v1/:accountId/blocklist */
void eos_blocklist(struct mg_connection *c, struct mg_http_message *hm);

/* PATCH /epic/presence/v1/... */
void eos_presence(struct mg_connection *c, struct mg_http_message *hm);

/* GET /epic/chat/v1/public/_/users/:accountId/summary */
void eos_chat_summary(struct mg_connection *c, struct mg_http_message *hm);

/* GET /epic/chat/v1/public/_/users/hybrid/conversations* */
void eos_chat_conversations(struct mg_connection *c, struct mg_http_message *hm);

/* POST /telemetry/data* */
void eos_telemetry(struct mg_connection *c, struct mg_http_message *hm);

/* POST /datarouter/api/v1/public/data/clients */
void eos_datarouter(struct mg_connection *c, struct mg_http_message *hm);

/* POST /user/v9/product-users/search */
void eos_product_users_search(struct mg_connection *c, struct mg_http_message *hm);

#endif
