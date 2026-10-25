#include <string.h>
#include "router.h"
#include "auth.h"
#include "eos.h"
#include "mcp.h"
#include "friends.h"
#include "locker.h"
#include "shop.h"
#include "cloudstorage.h"
#include "errors.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/* Simple pattern matching helpers                                      */
/* ------------------------------------------------------------------ */

/* Returns 1 if uri starts with prefix */
static int starts_with(const char *uri, const char *prefix) {
    return strncmp(uri, prefix, strlen(prefix)) == 0;
}

/* Returns 1 if uri contains substr */
static int contains(const char *uri, const char *substr) {
    return strstr(uri, substr) != NULL;
}

/* Returns 1 if method matches */
static int method_is(struct mg_http_message *hm, const char *m) {
    return mg_strcasecmp(hm->method, mg_str(m)) == 0;
}

/* ------------------------------------------------------------------ */
/* Generic stub responses                                               */
/* ------------------------------------------------------------------ */

static void send_empty_ok(struct mg_connection *c) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{}");
}

static void send_no_content(struct mg_connection *c) {
    mg_http_reply(c, 204, "", "");
}

static void send_ok_status(struct mg_connection *c) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"status\":\"OK\"}");
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                           */
/* ------------------------------------------------------------------ */

void router_dispatch(struct mg_connection *c, struct mg_http_message *hm) {
    /* Extract method and URI as null-terminated strings */
    char method[16] = {0};
    char uri[1024]  = {0};

    size_t mn = hm->method.len < sizeof(method)-1 ? hm->method.len : sizeof(method)-1;
    size_t un = hm->uri.len    < sizeof(uri)-1    ? hm->uri.len    : sizeof(uri)-1;
    memcpy(method, hm->method.buf, mn);
    memcpy(uri,    hm->uri.buf,    un);

    /* ---- AUTH ---------------------------------------------------- */
    if (strcmp(uri, "/account/api/oauth/token") == 0 && method_is(hm, "POST")) {
        auth_oauth_token(c, hm); return;
    }
    if (strcmp(uri, "/account/api/oauth/verify") == 0 && method_is(hm, "GET")) {
        auth_verify_token(c, hm); return;
    }
    if (starts_with(uri, "/account/api/oauth/sessions") && method_is(hm, "DELETE")) {
        send_no_content(c); return;
    }
    if (starts_with(uri, "/account/api/public/account/displayName/") && method_is(hm, "GET")) {
        auth_display_name(c, hm); return;
    }
    if (strcmp(uri, "/account/api/public/account/") == 0 && method_is(hm, "GET")) {
        auth_public_account(c, hm); return;
    }
    if (contains(uri, "/account/api/public/account/") &&
        contains(uri, "/externalAuths") && method_is(hm, "GET")) {
        auth_external_auths(c, hm); return;
    }
    if (starts_with(uri, "/account/api/public/account/") && method_is(hm, "GET")) {
        auth_account_info(c, hm); return;
    }
    if (starts_with(uri, "/fortnite/api/discovery/accessToken") && method_is(hm, "GET")) {
        auth_discovery_token(c, hm); return;
    }
    if (strcmp(uri, "/epic/oauth/v2/tokenInfo") == 0 && method_is(hm, "POST")) {
        auth_token_info(c, hm); return;
    }
    if (strcmp(uri, "/auth/v1/turn/credentials") == 0 && method_is(hm, "POST")) {
        auth_turn_credentials(c, hm); return;
    }
    if (strcmp(uri, "/publickey/v2/publickey") == 0 && method_is(hm, "POST")) {
        send_empty_ok(c); return;
    }

    /* ---- EOS ----------------------------------------------------- */
    if (strcmp(uri, "/auth/v1/oauth/token") == 0 && method_is(hm, "POST")) {
        eos_oauth_v1(c, hm); return;
    }
    if (strcmp(uri, "/epic/oauth/v2/token") == 0 && method_is(hm, "POST")) {
        eos_oauth_v2(c, hm); return;
    }
    if (strcmp(uri, "/epic/id/v2/sdk/accounts") == 0 && method_is(hm, "GET")) {
        eos_sdk_accounts(c, hm); return;
    }
    if (strcmp(uri, "/sdk/v1/default") == 0 && method_is(hm, "GET")) {
        eos_sdk_config(c, hm); return;
    }
    if (starts_with(uri, "/sdk/v1/product") && method_is(hm, "GET")) {
        eos_sdk_product(c, hm); return;
    }
    if (contains(uri, "/epic/friends/v1/") && contains(uri, "/blocklist") && method_is(hm, "GET")) {
        eos_blocklist(c, hm); return;
    }
    if (starts_with(uri, "/epic/presence/v1/")) {
        eos_presence(c, hm); return;
    }
    if (contains(uri, "/epic/chat/v1/") && contains(uri, "/summary") && method_is(hm, "GET")) {
        eos_chat_summary(c, hm); return;
    }
    if (contains(uri, "/epic/chat/v1/") && contains(uri, "/conversations")) {
        eos_chat_conversations(c, hm); return;
    }
    if (starts_with(uri, "/telemetry/data")) {
        eos_telemetry(c, hm); return;
    }
    if (strcmp(uri, "/datarouter/api/v1/public/data/clients") == 0 && method_is(hm, "POST")) {
        eos_datarouter(c, hm); return;
    }
    if (strcmp(uri, "/user/v9/product-users/search") == 0 && method_is(hm, "POST")) {
        eos_product_users_search(c, hm); return;
    }

    /* ---- MCP ----------------------------------------------------- */
    if (starts_with(uri, "/fortnite/api/game/v2/profile/") &&
        (contains(uri, "/client/") || contains(uri, "/dedicated_server/"))) {
        mcp_handler(c, hm); return;
    }

    /* ---- SHOP ---------------------------------------------------- */
    if (strcmp(uri, "/fortnite/api/storefront/v2/catalog") == 0 && method_is(hm, "GET")) {
        shop_catalog(c, hm); return;
    }
    if (strcmp(uri, "/fortnite/api/storefront/v2/keychain") == 0 && method_is(hm, "GET")) {
        shop_keychain(c, hm); return;
    }
    if (starts_with(uri, "/catalog/api/shared/bulk/offers")) {
        shop_bulk_offers(c, hm); return;
    }

    /* ---- FRIENDS ------------------------------------------------- */
    if (starts_with(uri, "/friends/api/public/friends/") && method_is(hm, "GET")) {
        friends_list(c, hm); return;
    }
    if (contains(uri, "/friends/api/v1/") && contains(uri, "/summary") && method_is(hm, "GET")) {
        friends_summary(c, hm); return;
    }
    if (contains(uri, "/friends/api/v1/") && contains(uri, "/settings") && method_is(hm, "GET")) {
        friends_settings(c, hm); return;
    }
    if (starts_with(uri, "/friends/api/public/blocklist/") && method_is(hm, "GET")) {
        friends_blocklist(c, hm); return;
    }
    if (contains(uri, "/friends/api/public/list/") && contains(uri, "/recentPlayers")) {
        friends_recent(c, hm); return;
    }
    if (contains(uri, "/api/statsv2/account/") && method_is(hm, "GET")) {
        friends_stats(c, hm); return;
    }
    if (contains(uri, "/fortnite/api/receipts/v1/account/") && contains(uri, "/receipts")) {
        friends_receipts(c, hm); return;
    }

    /* ---- PARTY / LOBBY ------------------------------------------- */
    if (starts_with(uri, "/party/api/v1/") && contains(uri, "/parties") &&
        !contains(uri, "/members") && method_is(hm, "POST")) {
        party_create(c, hm); return;
    }
    if (starts_with(uri, "/party/api/v1/Fortnite/user/") &&
        !contains(uri, "/pings") && !contains(uri, "/settings") &&
        !contains(uri, "/notifications") && method_is(hm, "GET")) {
        party_local(c, hm); return;
    }
    if (starts_with(uri, "/party/api/v1/") && contains(uri, "/parties/") &&
        !contains(uri, "/members") && !contains(uri, "/invites")) {
        party_get(c, hm); return;
    }
    if (starts_with(uri, "/party/api/v1/") && contains(uri, "/members/") &&
        contains(uri, "/join") && method_is(hm, "POST")) {
        party_join(c, hm); return;
    }
    if (starts_with(uri, "/party/api/v1/") && contains(uri, "/meta")) {
        party_meta(c, hm); return;
    }
    if (starts_with(uri, "/party/api/v1/") && contains(uri, "/pings/")) {
        party_pings(c, hm); return;
    }
    if (starts_with(uri, "/party/api/v1/Fortnite/user/") && contains(uri, "/settings/privacy")) {
        party_privacy(c, hm); return;
    }
    if (starts_with(uri, "/party/api/v1/") && contains(uri, "/notifications")) {
        /* Return empty notification count */
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"current\":[],\"pending\":[],\"invites\":[],\"pings\":[]}");
        return;
    }

    /* ---- LOCKER -------------------------------------------------- */
    if (starts_with(uri, "/api/locker/v3/") && contains(uri, "/items") && method_is(hm, "GET")) {
        locker_v3_get(c, hm); return;
    }
    if (starts_with(uri, "/api/locker/v3/") && contains(uri, "/loadout-preset/index/")) {
        locker_v3_preset(c, hm); return;
    }
    if (starts_with(uri, "/api/locker/v3/") && contains(uri, "/loadout/")) {
        locker_v3_set_loadout(c, hm); return;
    }
    if (starts_with(uri, "/api/locker/v4/") && contains(uri, "/cosmetic-data") && method_is(hm, "GET")) {
        locker_v4_cosmetic_data(c, hm); return;
    }
    if (starts_with(uri, "/api/locker/v4/") && contains(uri, "/items") && method_is(hm, "GET")) {
        locker_v4_get(c, hm); return;
    }
    if (starts_with(uri, "/api/locker/v4/") && contains(uri, "/active-loadout-group")) {
        locker_v4_set_loadout(c, hm); return;
    }
    if (starts_with(uri, "/api/locker/v4/") && contains(uri, "/loadout-group-preset/index/")) {
        locker_v4_group_preset(c, hm); return;
    }
    if (starts_with(uri, "/api/locker/v4/") && contains(uri, "/loadout-preset/index/")) {
        locker_v4_preset(c, hm); return;
    }
    if (starts_with(uri, "/api/locker/v4/") && contains(uri, "/lock-in-immutable-item/") && method_is(hm, "POST")) {
        locker_v4_immutable(c, hm); return;
    }
    if (starts_with(uri, "/api/locker/v4/") && contains(uri, "/companion-name")) {
        locker_v4_companion_name(c, hm); return;
    }

    /* ---- MISC stubs that the game always calls -------------------- */

    /* Splash screen "CHECKING EPIC SERVICES QUEUE". Epic/Neonite return
       204 when the queue is empty. A 404 makes the client retry forever. */
    if (contains(uri, "/waitingroom")) {
        send_no_content(c);
        return;
    }

    /* Bulk status wants an array; /service/:id/status wants an object. */
    if (starts_with(uri, "/lightswitch/api/service/bulk/status")) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "[{\"serviceInstanceId\":\"fortnite\",\"status\":\"UP\","
                      "\"message\":\"Cyber is UP\",\"maintenanceUri\":null,"
                      "\"overrideCatalogIds\":[\"a7f138b2e51945ffbfdacc1af0541053\"],"
                      "\"allowedActions\":[\"PLAY\",\"DOWNLOAD\"],\"banned\":false,"
                      "\"launcherInfoDTO\":{\"appName\":\"Fortnite\","
                      "\"catalogItemId\":\"4fe75bbc5a674f4f9b356b5c90567da5\","
                      "\"namespace\":\"fn\"}}]");
        return;
    }
    if (starts_with(uri, "/lightswitch/api/service")) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"serviceInstanceId\":\"fortnite\",\"status\":\"UP\","
                      "\"message\":\"Cyber is UP\",\"maintenanceUri\":null,"
                      "\"allowedActions\":[\"PLAY\",\"DOWNLOAD\"],\"banned\":false,"
                      "\"launcherInfoDTO\":{\"appName\":\"Fortnite\","
                      "\"catalogItemId\":\"4fe75bbc5a674f4f9b356b5c90567da5\","
                      "\"namespace\":\"fn\"}}");
        return;
    }
    if (starts_with(uri, "/fortnite/api/v2/versioncheck") ||
        starts_with(uri, "/fortnite/api/versioncheck")    ||
        strcmp(uri, "/fortnite/api/version") == 0) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"type\":\"NO_UPDATE\"}");
        return;
    }
    if (starts_with(uri, "/fortnite/api/game/v2/enabled_features")) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "[]"); return;
    }
    if (starts_with(uri, "/fortnite/api/calendar/v1/timeline")) {
        /* Minimal timeline so the game doesn't complain */
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"channels\":{\"client-matchmaking\":{\"states\":[],"
                      "\"cacheExpire\":\"9999-12-31T23:59:59.999Z\"},"
                      "\"client-events\":{\"states\":[{\"validFrom\":\"2020-01-01T00:00:00.000Z\","
                      "\"activeEvents\":[],\"state\":{\"activeStorefronts\":[],"
                      "\"eventNamedWeights\":{},\"seasonNumber\":7,"
                      "\"seasonTemplateId\":\"AthenaSeason:athenaseason7\","
                      "\"matchXpBonusPoints\":0,\"eventPunchCardTemplateId\":\"\","
                      "\"seasonBegin\":\"2020-01-01T00:00:00.000Z\","
                      "\"seasonEnd\":\"9999-12-31T23:59:59.000Z\","
                      "\"seasonDisplayedEnd\":\"9999-12-31T23:59:59.000Z\","
                      "\"weeklyStoreEnd\":\"9999-12-31T23:59:59.000Z\","
                      "\"stwEventStoreEnd\":\"9999-12-31T23:59:59.000Z\","
                      "\"stwWeeklyStoreEnd\":\"9999-12-31T23:59:59.000Z\","
                      "\"dailyStoreEnd\":\"9999-12-31T23:59:59.000Z\"}}],"
                      "\"cacheExpire\":\"9999-12-31T23:59:59.999Z\"}},"
                      "\"cacheIntervalMins\":10,"
                      "\"currentTime\":\"2026-01-01T00:00:00.000Z\"}");
        return;
    }
    if (starts_with(uri, "/fortnite/api/game/v2/privacy/account/")) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"accountId\":\"\",\"optOutOfPublicLeaderboards\":false}");
        return;
    }
    if (starts_with(uri, "/fortnite/api/game/v2/world/info")) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"theaters\":[],\"missions\":[],\"missionAlerts\":[]}");
        return;
    }
    if (starts_with(uri, "/fortnite/api/game/v2/br-inventory/account/")) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"stash\":{\"globalcash\":0}}");
        return;
    }
    if (starts_with(uri, "/socialban/api/public/v1/")) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"bans\":[],\"warnings\":[]}");
        return;
    }
    if (starts_with(uri, "/affiliate/api/public/affiliates/slug/")) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"id\":\"\",\"slug\":\"\",\"displayName\":\"\","
                      "\"status\":\"ACTIVE\",\"verified\":true}");
        return;
    }
    if (starts_with(uri, "/presence/api/v1/")) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "[]"); return;
    }
    if (starts_with(uri, "/eulatracking/")) {
        send_no_content(c); return;
    }
    if (starts_with(uri, "/datarouter/api/v1/public/data")) {
        send_no_content(c); return;
    }
    if (starts_with(uri, "/fortnite/api/game/v2/grant_access/")) {
        send_no_content(c); return;
    }
    if (starts_with(uri, "/fortnite/api/storeaccess/")) {
        send_no_content(c); return;
    }
    if (starts_with(uri, "/fortnite/api/game/v2/profileToken/verify/")) {
        send_no_content(c); return;
    }
    if (starts_with(uri, "/fortnite/api/game/v2/tryPlayOnPlatform/")) {
        mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "true"); return;
    }
    if (starts_with(uri, "/content/api/pages/fortnite-game")) {
        /* Minimal content pages response */
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"battleroyalenewsv2\":{\"news\":{\"messages\":[]},\"style\":\"Normal\"},"
                      "\"shopSections\":{\"sectionList\":{\"sections\":[]}},"
                      "\"_title\":\"fortnite-game\",\"_activeDate\":\"2020-01-01T00:00:00.000Z\","
                      "\"lastModified\":\"2020-01-01T00:00:00.000Z\"}");
        return;
    }
    if (starts_with(uri, "/api/v1/events/Fortnite/") && contains(uri, "/history/")) {
        send_empty_ok(c); return;
    }
    if (starts_with(uri, "/api/v1/events/Fortnite/download/")) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"events\":[]}"); return;
    }
    if (starts_with(uri, "/api/v1/assets/Fortnite/") && method_is(hm, "POST")) {
        send_empty_ok(c); return;
    }
    if (starts_with(uri, "/fortnite/api/cloudstorage/") ||
        starts_with(uri, "/api/v1/access/fortnite/cloudstorage/")) {
        cloudstorage_handler(c, hm);
        return;
    }
    if (starts_with(uri, "/v1/epic-settings/") ||
        starts_with(uri, "/api/v1/public/data/epic-settings")) {
        if (method_is(hm, "OPTIONS")) {
            send_no_content(c);
            return;
        }
        /* Newer clients reject a bare {}. Neonite returns a settings envelope. */
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"values\":{},\"response\":{\"settings\":[]}}");
        return;
    }

    /* ---- 404 catch-all ------------------------------------------- */
    cyber_warn("404 %s %s", method, uri);
    cyber_send_error(c, 404, ERR_NOT_FOUND,
                     "Sorry the resource you were trying to find could not be found", 1004);
}
