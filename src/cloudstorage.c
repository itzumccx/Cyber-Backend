#include "cloudstorage.h"
#include "../../vendor/cjson/cJSON.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define mkdir_compat(path) _mkdir(path)
#else
#include <dirent.h>
#include <sys/stat.h>
#define mkdir_compat(path) mkdir(path, 0755)
#endif
static void make_dirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char old = *p;
            *p = '\0';
            mkdir_compat(tmp);
            *p = old;
        }
    }
    mkdir_compat(tmp);
}

static int method_is(struct mg_http_message *hm, const char *m) {
    return mg_strcasecmp(hm->method, mg_str(m)) == 0;
}

static void hex_encode(const unsigned char *in, size_t n, char *out) {
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 15];
    }
    out[n * 2] = '\0';
}

static void hash_file(const unsigned char *data, size_t len,
                      char *sha1_hex, char *sha256_hex) {
    unsigned char s1[20], s2[32];
    mg_sha1_ctx a;
    mg_sha256_ctx b;
    mg_sha1_init(&a);
    mg_sha1_update(&a, data, len);
    mg_sha1_final(s1, &a);
    mg_sha256_init(&b);
    mg_sha256_update(&b, data, len);
    mg_sha256_final(s2, &b);
    hex_encode(s1, 20, sha1_hex);
    hex_encode(s2, 32, sha256_hex);
}

static void safe_name(const char *in, char *out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_len && j < 128; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    if (!out[0]) strncpy(out, "file", out_len - 1);
}

static void user_dir(const char *account_id, char *out, size_t out_len) {
    char acc[128];
    safe_name(account_id, acc, sizeof(acc));
    snprintf(out, out_len, "profile/%s/cloudstorage", acc);
}

static void send_bytes(struct mg_connection *c, const void *data, size_t len) {
    mg_printf(c,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/octet-stream\r\n"
              "Content-Length: %zu\r\n"
              "\r\n",
              len);
    if (len && data) mg_send(c, data, len);
}

static void send_json(struct mg_connection *c, const char *json) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json);
}

static void send_204(struct mg_connection *c) {
    mg_http_reply(c, 204, "", "");
}

/* Copy path after a prefix. Returns 1 if prefix matched. */
static int after_prefix(const char *uri, const char *prefix, char *rest, size_t rest_len) {
    size_t n = strlen(prefix);
    if (strncmp(uri, prefix, n) != 0) return 0;
    snprintf(rest, rest_len, "%s", uri + n);
    /* strip query if mongoose ever includes it */
    char *q = strchr(rest, '?');
    if (q) *q = '\0';
    return 1;
}

static void split2(const char *rest, char *a, size_t alen, char *b, size_t blen) {
    a[0] = b[0] = '\0';
    while (*rest == '/') rest++;
    const char *slash = strchr(rest, '/');
    if (!slash) {
        snprintf(a, alen, "%s", rest);
        return;
    }
    size_t n = (size_t)(slash - rest);
    if (n >= alen) n = alen - 1;
    memcpy(a, rest, n);
    a[n] = '\0';
    slash++;
    while (*slash == '/') slash++;
    snprintf(b, blen, "%s", slash);
    char *extra = strchr(b, '/');
    if (extra) *extra = '\0';
}

static unsigned char *read_bin(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return NULL; }
    unsigned char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    *out_len = n;
    return buf;
}

static void list_user_files(struct mg_connection *c, const char *account_id) {
    char dir[512];
    user_dir(account_id, dir, sizeof(dir));
    make_dirs(dir);

    cJSON *arr = cJSON_CreateArray();

#ifdef _WIN32
    char glob[540];
    snprintf(glob, sizeof(glob), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char path[540];
            snprintf(path, sizeof(path), "%s/%s", dir, fd.cFileName);
            size_t len = 0;
            unsigned char *data = read_bin(path, &len);
            if (!data) continue;
            char sha1[41], sha256[65];
            hash_file(data, len, sha1, sha256);
            free(data);

            SYSTEMTIME st;
            FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
            char uploaded[40];
            snprintf(uploaded, sizeof(uploaded),
                     "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "uniqueFilename", fd.cFileName);
            cJSON_AddStringToObject(item, "filename", fd.cFileName);
            cJSON_AddStringToObject(item, "hash", sha1);
            cJSON_AddStringToObject(item, "hash256", sha256);
            cJSON_AddNumberToObject(item, "length", (double)len);
            cJSON_AddStringToObject(item, "contentType", "application/octet-stream");
            cJSON_AddStringToObject(item, "uploaded", uploaded);
            cJSON_AddStringToObject(item, "storageType", "S3");
            cJSON_AddFalseToObject(item, "doNotCache");
            cJSON_AddItemToArray(arr, item);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            char path[540];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            size_t len = 0;
            unsigned char *data = read_bin(path, &len);
            if (!data) continue;
            char sha1[41], sha256[65];
            hash_file(data, len, sha1, sha256);
            free(data);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "uniqueFilename", ent->d_name);
            cJSON_AddStringToObject(item, "filename", ent->d_name);
            cJSON_AddStringToObject(item, "hash", sha1);
            cJSON_AddStringToObject(item, "hash256", sha256);
            cJSON_AddNumberToObject(item, "length", (double)len);
            cJSON_AddStringToObject(item, "contentType", "application/octet-stream");
            cJSON_AddStringToObject(item, "uploaded", "2020-01-01T00:00:00.000Z");
            cJSON_AddStringToObject(item, "storageType", "S3");
            cJSON_AddFalseToObject(item, "doNotCache");
            cJSON_AddItemToArray(arr, item);
        }
        closedir(d);
    }
#endif

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    send_json(c, json);
    free(json);
}

static void get_user_file(struct mg_connection *c, const char *account_id, const char *file_name) {
    char dir[512], path[640], fname[160];
    user_dir(account_id, dir, sizeof(dir));
    safe_name(file_name, fname, sizeof(fname));
    snprintf(path, sizeof(path), "%s/%s", dir, fname);

    size_t len = 0;
    unsigned char *data = read_bin(path, &len);
    /* Empty 200 (not 404): newer clients show CLOUD DOWNLOAD FAILURE on 404. */
    if (!data) {
        send_bytes(c, "", 0);
        return;
    }
    send_bytes(c, data, len);
    free(data);
}

static void put_user_file(struct mg_connection *c, struct mg_http_message *hm,
                          const char *account_id, const char *file_name) {
    char dir[512], path[640], fname[160];
    user_dir(account_id, dir, sizeof(dir));
    make_dirs(dir);
    safe_name(file_name, fname, sizeof(fname));
    snprintf(path, sizeof(path), "%s/%s", dir, fname);

    if (hm->body.len > 1000000) {
        mg_http_reply(c, 413, "Content-Type: application/json\r\n",
                      "{\"errorCode\":\"errors.com.epicgames.cloudstorage.file_too_large\"}");
        return;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                      "{\"error\":\"upload failed\"}");
        return;
    }
    if (hm->body.len) fwrite(hm->body.buf, 1, hm->body.len, f);
    fclose(f);
    send_204(c);
}

void cloudstorage_handler(struct mg_connection *c, struct mg_http_message *hm) {
    char uri[1024] = {0};
    size_t un = hm->uri.len < sizeof(uri) - 1 ? hm->uri.len : sizeof(uri) - 1;
    memcpy(uri, hm->uri.buf, un);

    char rest[768];

    if (after_prefix(uri, "/fortnite/api/cloudstorage/system/config", rest, sizeof(rest))) {
        send_204(c);
        return;
    }
    if (strcmp(uri, "/fortnite/api/cloudstorage/system") == 0) {
        send_json(c, "[]");
        return;
    }
    if (after_prefix(uri, "/fortnite/api/cloudstorage/system/", rest, sizeof(rest))) {
        /* Hotfixes not required to get past the Client Settings popup. */
        send_bytes(c, "", 0);
        return;
    }
    if (strcmp(uri, "/fortnite/api/cloudstorage/user/config") == 0) {
        send_json(c, "[]");
        return;
    }

    if (after_prefix(uri, "/fortnite/api/cloudstorage/user/", rest, sizeof(rest)) ||
        after_prefix(uri, "/api/v1/access/fortnite/cloudstorage/Live/user/", rest, sizeof(rest))) {
        char account_id[128], file_name[256];
        split2(rest, account_id, sizeof(account_id), file_name, sizeof(file_name));
        if (!account_id[0]) {
            send_json(c, "[]");
            return;
        }
        if (!file_name[0]) {
            list_user_files(c, account_id);
            return;
        }
        if (method_is(hm, "PUT") || method_is(hm, "POST")) {
            put_user_file(c, hm, account_id, file_name);
            return;
        }
        if (method_is(hm, "DELETE")) {
            char dir[512], path[640], fname[160];
            user_dir(account_id, dir, sizeof(dir));
            safe_name(file_name, fname, sizeof(fname));
            snprintf(path, sizeof(path), "%s/%s", dir, fname);
            remove(path);
            send_204(c);
            return;
        }
        get_user_file(c, account_id, file_name);
        return;
    }

    send_json(c, "[]");
}
