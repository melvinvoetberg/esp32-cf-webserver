#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp32_cf.h"

/* ---- Configuration (injected via env vars — see .env.example) ---- */
#define WIFI_RETRIES 5

static const char *TAG = "main";
static int s_retry = 0;

static inline const char *env_or_null(const char *s) {
    return (s && s[0] != '\0') ? s : NULL;
}

/* ---- Static file server ---- */

static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcmp(ext, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(ext, ".css"))  return "text/css";
    if (!strcmp(ext, ".js"))   return "application/javascript";
    if (!strcmp(ext, ".json")) return "application/json";
    if (!strcmp(ext, ".png"))  return "image/png";
    if (!strcmp(ext, ".ico"))  return "image/x-icon";
    if (!strcmp(ext, ".svg"))  return "image/svg+xml";
    return "application/octet-stream";
}

/* Simple {{key}} template engine */
typedef struct { const char *key; char val[64]; } tpl_var_t;

static void build_vars(tpl_var_t *vars, int *n) {
    *n = 0;
    cf_tunnel_status_t st;
    cf_tunnel_get_status(&st);

    vars[*n].key = "server.hostname";
    const char *h = cf_tunnel_get_hostname();
    snprintf(vars[*n].val, sizeof(vars[*n].val), "%s", h ? h : "?");
    (*n)++;

    vars[*n].key = "server.edge";
    snprintf(vars[*n].val, sizeof(vars[*n].val), "%s", st.edge_location);
    (*n)++;

    vars[*n].key = "server.heap";
    snprintf(vars[*n].val, sizeof(vars[*n].val), "%lu", (unsigned long)esp_get_free_heap_size());
    (*n)++;

    vars[*n].key = "server.requests";
    snprintf(vars[*n].val, sizeof(vars[*n].val), "%lu", (unsigned long)st.requests_handled);
    (*n)++;

    vars[*n].key = "server.uptime";
    snprintf(vars[*n].val, sizeof(vars[*n].val), "%lld", (long long)(esp_timer_get_time() / 1000000));
    (*n)++;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        vars[*n].key = "server.wifi.ssid";
        snprintf(vars[*n].val, sizeof(vars[*n].val), "%s", (char *)ap.ssid);
        (*n)++;
        vars[*n].key = "server.wifi.rssi";
        snprintf(vars[*n].val, sizeof(vars[*n].val), "%d", ap.rssi);
        (*n)++;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        vars[*n].key = "server.ip";
        snprintf(vars[*n].val, sizeof(vars[*n].val), IPSTR, IP2STR(&ip_info.ip));
        (*n)++;
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    vars[*n].key = "server.chip";
    const char *name;
    switch (chip.model) {
        case 1:  name = "ESP32";    break;
        case 2:  name = "ESP32-S2"; break;
        case 9:  name = "ESP32-S3"; break;
        case 5:  name = "ESP32-C3"; break;
        case 12: name = "ESP32-C2"; break;
        case 13: name = "ESP32-C6"; break;
        case 16: name = "ESP32-H2"; break;
        default: name = "ESP32";    break;
    }
    snprintf(vars[*n].val, sizeof(vars[*n].val), "%s (rev %d.%d)",
             name, chip.revision / 100, chip.revision % 100);
    (*n)++;
}

static size_t render(const char *tpl, size_t tlen, char *out, size_t ocap,
                     const tpl_var_t *vars, int nvars) {
    size_t op = 0, ip = 0;
    while (ip < tlen && op < ocap - 1) {
        if (ip + 3 < tlen && tpl[ip] == '{' && tpl[ip + 1] == '{') {
            const char *end = strstr(tpl + ip + 2, "}}");
            if (end) {
                size_t klen = end - (tpl + ip + 2);
                for (int i = 0; i < nvars; i++) {
                    if (strlen(vars[i].key) == klen &&
                        memcmp(vars[i].key, tpl + ip + 2, klen) == 0) {
                        size_t vlen = strlen(vars[i].val);
                        if (op + vlen < ocap - 1) {
                            memcpy(out + op, vars[i].val, vlen);
                            op += vlen;
                        }
                        break;
                    }
                }
                ip = (end - tpl) + 2;
                continue;
            }
        }
        out[op++] = tpl[ip++];
    }
    out[op] = 0;
    return op;
}

#define MAX_FILE_SIZE   8192
#define MAX_RENDER_SIZE 10240

static void serve(cf_edge_conn_t *conn, int32_t sid,
                  const char *method, const char *path, const char *client_ip) {
    char fpath[128];
    if (!strcmp(path, "/") || !strcmp(path, ""))
        snprintf(fpath, sizeof(fpath), "/www/index.html");
    else
        snprintf(fpath, sizeof(fpath), "/www%s", path);

    ESP_LOGI(TAG, "%s %s %s", client_ip, method, path);

    FILE *f = fopen(fpath, "r");
    if (!f) {
        const char *msg = "404 Not Found";
        cf_tunnel_respond(conn, sid, 404, "text/plain", (uint8_t *)msg, strlen(msg));
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > MAX_FILE_SIZE) {
        fclose(f);
        const char *msg = "500 File too large";
        cf_tunnel_respond(conn, sid, 500, "text/plain", (uint8_t *)msg, strlen(msg));
        return;
    }

    char *fbuf = malloc(fsize + 1);
    if (!fbuf) { fclose(f); return; }
    fread(fbuf, 1, fsize, f);
    fbuf[fsize] = 0;
    fclose(f);

    const char *ct = mime_type(fpath);
    const char *ext = strrchr(fpath, '.');
    bool do_template = ext && (!strcmp(ext, ".html") || !strcmp(ext, ".json"));

    if (do_template) {
        tpl_var_t vars[12];
        int nv;
        build_vars(vars, &nv);
        char *out = malloc(MAX_RENDER_SIZE);
        if (out) {
            size_t olen = render(fbuf, fsize, out, MAX_RENDER_SIZE, vars, nv);
            cf_tunnel_respond(conn, sid, 200, ct, (uint8_t *)out, olen);
            free(out);
        }
    } else {
        cf_tunnel_respond(conn, sid, 200, ct, (uint8_t *)fbuf, fsize);
    }
    free(fbuf);
}

/* ---- Request handler ---- */

static void on_request(cf_edge_conn_t *conn, int32_t stream_id,
                       const char *method, const char *path,
                       const char *host, const char *client_ip, void *ud) {
    serve(conn, stream_id, method, path, client_ip);
}

/* ---- Tunnel ---- */

static void start_tunnel(void) {
    /* Mount LittleFS for static files */
    esp_vfs_littlefs_conf_t fs = {
        .base_path = "/www",
        .partition_label = "www",
        .format_if_mount_failed = true,
    };
    if (esp_vfs_littlefs_register(&fs) != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed");
        return;
    }

    cf_tunnel_config_t cfg = CF_TUNNEL_CONFIG_DEFAULT();
    cfg.tunnel_token  = env_or_null(CF_TUNNEL_TOKEN);
    cfg.hostname      = env_or_null(CF_HOSTNAME);
    cfg.connector_id  = env_or_null(CF_CONNECTOR_ID);
    cfg.on_request    = on_request;

    if (cf_tunnel_init(&cfg) != ESP_OK) return;
    cf_tunnel_start();
}

/* ---- WiFi ---- */

static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < WIFI_RETRIES) { esp_wifi_connect(); s_retry++; }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
        start_tunnel();
    }
}

static void wifi_init(void) {
    if (sizeof(WIFI_SSID) <= 1) {
        ESP_LOGE(TAG, "WIFI_SSID not set! Export WIFI_SSID env var before building.");
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    esp_event_handler_instance_t a, b;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_handler, NULL, &a));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_handler, NULL, &b));
    wifi_config_t wf = {.sta = {.ssid = WIFI_SSID, .password = WIFI_PASS}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wf));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    wifi_init();
}
