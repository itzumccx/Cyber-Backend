#ifndef CYBER_CLOUDSTORAGE_H
#define CYBER_CLOUDSTORAGE_H

#include "../../vendor/mongoose/mongoose.h"

void cloudstorage_handler(struct mg_connection *c, struct mg_http_message *hm);

#endif
