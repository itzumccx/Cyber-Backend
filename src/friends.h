#ifndef CYBER_FRIENDS_H
#define CYBER_FRIENDS_H

#include "../../vendor/mongoose/mongoose.h"

void friends_list(struct mg_connection *c, struct mg_http_message *hm);
void friends_summary(struct mg_connection *c, struct mg_http_message *hm);
void friends_settings(struct mg_connection *c, struct mg_http_message *hm);
void friends_blocklist(struct mg_connection *c, struct mg_http_message *hm);
void friends_recent(struct mg_connection *c, struct mg_http_message *hm);
void friends_stats(struct mg_connection *c, struct mg_http_message *hm);
void friends_receipts(struct mg_connection *c, struct mg_http_message *hm);

void party_create(struct mg_connection *c, struct mg_http_message *hm);
void party_local(struct mg_connection *c, struct mg_http_message *hm);
void party_get(struct mg_connection *c, struct mg_http_message *hm);
void party_join(struct mg_connection *c, struct mg_http_message *hm);
void party_meta(struct mg_connection *c, struct mg_http_message *hm);
void party_pings(struct mg_connection *c, struct mg_http_message *hm);
void party_privacy(struct mg_connection *c, struct mg_http_message *hm);

#endif
