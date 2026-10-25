#ifndef CYBER_AUTH_H
#define CYBER_AUTH_H

#include "../../vendor/mongoose/mongoose.h"

/* POST /account/api/oauth/token */
void auth_oauth_token(struct mg_connection *c, struct mg_http_message *hm);

/* GET /account/api/oauth/verify */
void auth_verify_token(struct mg_connection *c, struct mg_http_message *hm);

/* GET /account/api/public/account/:accountId */
void auth_account_info(struct mg_connection *c, struct mg_http_message *hm);

/* GET /account/api/public/account/displayName/:displayName */
void auth_display_name(struct mg_connection *c, struct mg_http_message *hm);

/* GET /account/api/public/account/ (query param accountId) */
void auth_public_account(struct mg_connection *c, struct mg_http_message *hm);

/* GET /account/api/public/account/:accountId/externalAuths */
void auth_external_auths(struct mg_connection *c, struct mg_http_message *hm);

void auth_discovery_token(struct mg_connection *c, struct mg_http_message *hm);

/* POST /epic/oauth/v2/tokenInfo */
void auth_token_info(struct mg_connection *c, struct mg_http_message *hm);

/* POST /auth/v1/turn/credentials */
void auth_turn_credentials(struct mg_connection *c, struct mg_http_message *hm);

#endif
