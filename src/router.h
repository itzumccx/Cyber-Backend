#ifndef CYBER_ROUTER_H
#define CYBER_ROUTER_H

#include "../../vendor/mongoose/mongoose.h"

/* Called by main's mongoose event handler for every HTTP request */
void router_dispatch(struct mg_connection *c, struct mg_http_message *hm);

#endif
