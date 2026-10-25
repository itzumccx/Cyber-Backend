#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "router.h"
#include "log.h"
#include "../../vendor/mongoose/mongoose.h"
#include "../../vendor/cjson/cJSON.h"

/* ------------------------------------------------------------------ */
/* Config                                                               */
/* ------------------------------------------------------------------ */

static int  g_port     = 5595;   /* same port as Neonite / Carbon launcher */
static int  g_running  = 1;

static void load_config(void) {
    FILE *f = fopen("config.json", "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *raw = malloc(sz + 1);
    fread(raw, 1, sz, f);
    raw[sz] = '\0';
    fclose(f);

    cJSON *cfg = cJSON_Parse(raw);
    free(raw);
    if (!cfg) { cyber_warn("config.json is invalid JSON, using defaults"); return; }

    cJSON *port = cJSON_GetObjectItem(cfg, "port");
    if (cJSON_IsNumber(port)) g_port = port->valueint;

    cJSON_Delete(cfg);
}

/* ------------------------------------------------------------------ */
/* Mongoose event handler                                               */
/* ------------------------------------------------------------------ */

static void event_handler(struct mg_connection *c, int ev,
                           void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        /* Log the incoming request */
        char uri[256] = {0};
        size_t n = hm->uri.len < sizeof(uri) - 1 ? hm->uri.len : sizeof(uri) - 1;
        memcpy(uri, hm->uri.buf, n);

        char method[16] = {0};
        n = hm->method.len < sizeof(method) - 1 ? hm->method.len : sizeof(method) - 1;
        memcpy(method, hm->method.buf, n);

        /* Host helps confirm the launcher is actually hitting this process */
        char host[256] = {0};
        struct mg_str *hhost = mg_http_get_header(hm, "Host");
        if (hhost && hhost->len) {
            size_t hn = hhost->len < sizeof(host) - 1 ? hhost->len : sizeof(host) - 1;
            memcpy(host, hhost->buf, hn);
        }
        cyber_log("%s %s  host=%s", method, uri, host[0] ? host : "(none)");

        /* Dispatch to the right handler */
        router_dispatch(c, hm);
    }
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                       */
/* ------------------------------------------------------------------ */

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int main(void) {
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    load_config();

    printf("=================================\n");
    printf("  Cyber - Fortnite Backend\n");
    printf("=================================\n");

    char listen_addr[32];
    snprintf(listen_addr, sizeof(listen_addr), "0.0.0.0:%d", g_port);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    struct mg_connection *conn = mg_http_listen(&mgr, listen_addr, event_handler, NULL);
    if (!conn) {
        cyber_error("Failed to listen on %s", listen_addr);
        cyber_error("Port %d may already be in use.", g_port);
        mg_mgr_free(&mgr);
        return 1;
    }

    cyber_log("Listening on port %d", g_port);
    cyber_log("Profiles stored in ./profile/");
    cyber_log("Shop loaded from ./responses/catalog/shop.json");
    cyber_log("Press Ctrl+C to stop");
    printf("---------------------------------\n");

    while (g_running) {
        mg_mgr_poll(&mgr, 100);
    }

    cyber_log("Shutting down...");
    mg_mgr_free(&mgr);
    return 0;
}
