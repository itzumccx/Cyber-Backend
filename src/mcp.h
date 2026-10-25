#ifndef CYBER_MCP_H
#define CYBER_MCP_H

#include "../../vendor/mongoose/mongoose.h"

/* POST /fortnite/api/game/v2/profile/:accountId/client/:command
   POST /fortnite/api/game/v2/profile/:accountId/dedicated_server/:command */
void mcp_handler(struct mg_connection *c, struct mg_http_message *hm);

#endif
