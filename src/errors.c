#include <stdio.h>
#include <string.h>
#include "errors.h"
#include "log.h"

void cyber_send_error(struct mg_connection *c,
                      int http_status,
                      const char *error_code,
                      const char *error_message,
                      int numeric_code) {

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "errorCode", error_code);
    cJSON_AddStringToObject(root, "errorMessage", error_message);
    cJSON_AddNumberToObject(root, "numericErrorCode", numeric_code);
    cJSON_AddStringToObject(root, "originatingService", "cyber");
    cJSON_AddStringToObject(root, "intent", "prod-live");

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    mg_http_reply(c, http_status,
        "Content-Type: application/json\r\n",
        "%s", body);

    free(body);
}
