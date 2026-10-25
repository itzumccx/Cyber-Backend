#ifndef CYBER_SHOP_H
#define CYBER_SHOP_H

#include "../../vendor/mongoose/mongoose.h"

/* GET /fortnite/api/storefront/v2/catalog */
void shop_catalog(struct mg_connection *c, struct mg_http_message *hm);

/* GET /fortnite/api/storefront/v2/keychain */
void shop_keychain(struct mg_connection *c, struct mg_http_message *hm);

/* GET /catalog/api/shared/bulk/offers */
void shop_bulk_offers(struct mg_connection *c, struct mg_http_message *hm);

#endif
