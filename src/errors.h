#ifndef CYBER_ERRORS_H
#define CYBER_ERRORS_H

#include "../../vendor/mongoose/mongoose.h"
#include "../../vendor/cjson/cJSON.h"

/* Send a standard Epic Games error JSON response */
void cyber_send_error(struct mg_connection *c,
                      int http_status,
                      const char *error_code,
                      const char *error_message,
                      int numeric_code);

/* Common error codes */
#define ERR_NOT_FOUND          "errors.com.epicgames.common.not_found"
#define ERR_SERVER_ERROR       "errors.com.epicgames.common.server_error"
#define ERR_OAUTH_INVALID      "errors.com.epicgames.common.oauth.invalid_request"
#define ERR_OAUTH_UNSUPPORTED  "errors.com.epicgames.common.oauth.unsupported_grant_type"
#define ERR_ITEM_NOT_FOUND     "errors.com.epicgames.fortnite.item_not_found"
#define ERR_OP_NOT_FOUND       "errors.com.epicgames.fortnite.operation_not_found"
#define ERR_PROFILE_FORBIDDEN  "errors.com.epicgames.modules.profiles.operation_forbidden"
#define ERR_CATALOG_OUTDATED   "errors.com.epicgames.modules.gamesubcatalog.catalog_out_of_date"

#endif
