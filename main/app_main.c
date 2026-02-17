/*
 * ESP32-P4 WiFi Streaming Test
 *
 * Connects to WiFi as STA via esp-hosted (ESP32-C6 over SDIO),
 * then streams UDP to a target host to measure real throughput.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "wifi_raw.h"

static const char *TAG = "wifi_stream";

/* ─── WiFi Configuration ─── */
#define WIFI_SSID_PRIMARY     "MALARnetJio_EXT"
#define WIFI_SSID_FALLBACK    "MALARnet"
#define WIFI_PASS             "Peter@1954"
#define WIFI_MAX_RETRY        5

/* ─── Streaming Configuration ─── */
#define TARGET_IP             "192.168.1.128"
#define TARGET_PORT           5001
#define TX_PACKET_SIZE        1400
#define TEST_DURATION_SEC     30
#define STATS_INTERVAL_MS     1000

/* ─── WiFi Event Handling ─── */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1

static int s_retry_num = 0;
static char s_connected_ssid[33] = {0};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry WiFi connection (%d/%d)...", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "[OK] Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ─── WiFi STA Init & Connect ─── */
static esp_err_t wifi_connect(const char *ssid)
{
    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, WIFI_PASS, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        strncpy(s_connected_ssid, ssid, sizeof(s_connected_ssid) - 1);
        return ESP_OK;
    }

    /* Stop before trying next SSID */
    esp_wifi_stop();
    return ESP_FAIL;
}

static esp_err_t wifi_init_sta(void)
{
    ESP_LOGI(TAG, "════════════════════════════════════════");
    ESP_LOGI(TAG, "  Phase 1: WiFi STA Initialization");
    ESP_LOGI(TAG, "════════════════════════════════════════");

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[OK] NVS initialized");

    /* Network stack */
    ESP_ERROR_CHECK(esp_netif_init());
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    ESP_LOGI(TAG, "[OK] Network stack initialized");

    /* Create STA netif */
    esp_netif_create_default_wifi_sta();
    ESP_LOGI(TAG, "[OK] STA netif created");

    /* WiFi init */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] esp_wifi_init: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }
    ESP_LOGI(TAG, "[OK] esp_wifi_init succeeded");

    /* Register event handlers */
    s_wifi_event_group = xEventGroupCreate();
    esp_event_handler_instance_t inst_any_id, inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_got_ip));

    /* Set STA mode (HT20 — HT40 tested but worse due to 2.4GHz congestion) */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Try primary SSID, then fallback */
    ret = wifi_connect(WIFI_SSID_PRIMARY);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "'%s' failed, trying '%s'...", WIFI_SSID_PRIMARY, WIFI_SSID_FALLBACK);
        ret = wifi_connect(WIFI_SSID_FALLBACK);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] Could not connect to any WiFi network");
        return ret;
    }

    /* Read back info */
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(TAG, "[OK] STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "[OK] Connected to '%s' (ch:%d, rssi:%d)",
                 s_connected_ssid, ap_info.primary, ap_info.rssi);
    }

    ESP_LOGI(TAG, "Phase 1 COMPLETE");
    return ESP_OK;
}

/* ─── Counters ─── */
static volatile uint32_t s_tx_packets = 0;
static volatile uint64_t s_tx_bytes = 0;
static volatile uint32_t s_tx_errors = 0;
static volatile bool s_tx_running = false;

static void reset_counters(void)
{
    s_tx_packets = 0;
    s_tx_bytes = 0;
    s_tx_errors = 0;
}

static void print_tx_stats(int elapsed_sec)
{
    uint32_t pkts = s_tx_packets;
    uint64_t bytes = s_tx_bytes;
    uint32_t errs = s_tx_errors;
    float mbps = (elapsed_sec > 0) ? (bytes * 8.0f / 1000000.0f / elapsed_sec) : 0;
    float pps = (elapsed_sec > 0) ? ((float)pkts / elapsed_sec) : 0;

    ESP_LOGI(TAG, "  [%2ds] %6lu pkts (%4.0f pps) | %6.2f Mbps | err:%lu",
             elapsed_sec,
             (unsigned long)pkts, pps, mbps, (unsigned long)errs);
}

/* ─── UDP Streaming Task ─── */
static void udp_stream_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(TARGET_PORT),
    };
    inet_aton(TARGET_IP, &dest.sin_addr);

    /* Increase send buffer */
    int sndbuf = 65536;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    uint8_t *buf = malloc(TX_PACKET_SIZE);
    if (!buf) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    /* Fill with pattern */
    for (int i = 0; i < TX_PACKET_SIZE; i++)
        buf[i] = (uint8_t)(i & 0xFF);

    ESP_LOGI(TAG, "UDP stream started -> %s:%d (%d byte packets)",
             TARGET_IP, TARGET_PORT, TX_PACKET_SIZE);

    while (s_tx_running) {
        int sent = sendto(sock, buf, TX_PACKET_SIZE, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
        if (sent > 0) {
            s_tx_packets++;
            s_tx_bytes += sent;
        } else {
            s_tx_errors++;
            if (errno == ENOMEM || errno == EAGAIN) {
                taskYIELD();
            }
        }
    }

    free(buf);
    close(sock);
    ESP_LOGI(TAG, "UDP stream task stopped");
    vTaskDelete(NULL);
}

static void test_udp_stream(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "════════════════════════════════════════");
    ESP_LOGI(TAG, "  UDP Stream to %s:%d (%ds)", TARGET_IP, TARGET_PORT, TEST_DURATION_SEC);
    ESP_LOGI(TAG, "════════════════════════════════════════");

    reset_counters();
    s_tx_running = true;

    xTaskCreatePinnedToCore(udp_stream_task, "udp_tx", 4096, NULL,
                            configMAX_PRIORITIES - 2, NULL, 0);

    for (int sec = 1; sec <= TEST_DURATION_SEC; sec++) {
        vTaskDelay(pdMS_TO_TICKS(STATS_INTERVAL_MS));
        print_tx_stats(sec);
    }

    s_tx_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));

    float mbps = s_tx_bytes * 8.0f / 1000000.0f / TEST_DURATION_SEC;

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  RESULT: %.2f Mbps (%lu pkts, %lu err)  ║",
             mbps, (unsigned long)s_tx_packets, (unsigned long)s_tx_errors);
    ESP_LOGI(TAG, "║  Target: %s:%d via '%s'  ║",
             TARGET_IP, TARGET_PORT, s_connected_ssid);
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════╝");
}

/* ─── Packet Monitor Test ─── */
static volatile uint32_t s_mon_mgmt = 0;
static volatile uint32_t s_mon_ctrl = 0;
static volatile uint32_t s_mon_data = 0;
static volatile uint32_t s_mon_misc = 0;

static void monitor_rx_cb(const wifi_raw_rx_pkt_t *pkt)
{
    /* Count by type */
    switch (pkt->type) {
        case 0: s_mon_mgmt++; break;  /* WIFI_PKT_MGMT */
        case 1: s_mon_ctrl++; break;  /* WIFI_PKT_CTRL */
        case 2: s_mon_data++; break;  /* WIFI_PKT_DATA */
        default: s_mon_misc++; break; /* WIFI_PKT_MISC */
    }

    /* Log first bytes of management frames for beacon/probe detection */
    if (pkt->type == 0 && pkt->payload_len >= 24) {
        uint8_t subtype = (pkt->payload[0] >> 4) & 0x0F;
        const char *name = "other";
        switch (subtype) {
            case 0: name = "assoc_req"; break;
            case 1: name = "assoc_resp"; break;
            case 4: name = "probe_req"; break;
            case 5: name = "probe_resp"; break;
            case 8: name = "beacon"; break;
            case 10: name = "disassoc"; break;
            case 11: name = "auth"; break;
            case 12: name = "deauth"; break;
        }
        /* Only log non-beacon frames or every 100th beacon to avoid flooding */
        if (subtype != 8 || (s_mon_mgmt % 100) == 1) {
            ESP_LOGI("monitor", "MGMT %s ch:%d rssi:%d len:%d",
                     name, pkt->channel, pkt->rssi, pkt->payload_len);
        }
    }
}

static void test_packet_monitor(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "════════════════════════════════════════");
    ESP_LOGI(TAG, "  Phase 3: Packet Monitor Test (10s)");
    ESP_LOGI(TAG, "════════════════════════════════════════");

    /* Init wifi_raw */
    esp_err_t ret = wifi_raw_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_raw_init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Register RX callback */
    wifi_raw_register_rx_cb(monitor_rx_cb);

    /* Set filter: management + data frames */
    ret = wifi_raw_set_filter(0x0F);  /* WIFI_PROMIS_FILTER_MASK_ALL */
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Set filter: %s (continuing anyway)", esp_err_to_name(ret));
    }

    /* Enable promiscuous mode */
    s_mon_mgmt = 0;
    s_mon_ctrl = 0;
    s_mon_data = 0;
    s_mon_misc = 0;

    ret = wifi_raw_set_promiscuous(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable promiscuous mode failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Promiscuous mode ENABLED - capturing packets...");

    /* Monitor for 10 seconds with stats every second */
    for (int sec = 1; sec <= 10; sec++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "  [%2ds] mgmt:%lu ctrl:%lu data:%lu misc:%lu",
                 sec,
                 (unsigned long)s_mon_mgmt, (unsigned long)s_mon_ctrl,
                 (unsigned long)s_mon_data, (unsigned long)s_mon_misc);
    }

    /* Disable promiscuous mode */
    ret = wifi_raw_set_promiscuous(false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Disable promiscuous mode: %s", esp_err_to_name(ret));
    }

    uint32_t total = s_mon_mgmt + s_mon_ctrl + s_mon_data + s_mon_misc;
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  MONITOR RESULT: %lu packets captured        ║", (unsigned long)total);
    ESP_LOGI(TAG, "║  MGMT:%lu CTRL:%lu DATA:%lu MISC:%lu         ║",
             (unsigned long)s_mon_mgmt, (unsigned long)s_mon_ctrl,
             (unsigned long)s_mon_data, (unsigned long)s_mon_misc);
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════╝");
}

/* ─── Main ─── */
void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ESP32-P4 WiFi Streaming Test via esp-hosted     ║");
    ESP_LOGI(TAG, "║  UDP stream to %s:%d              ║", TARGET_IP, TARGET_PORT);
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    /* Phase 1: Connect to WiFi */
    esp_err_t ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed. Halting.");
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Phase 2: UDP stream throughput test */
    test_udp_stream();

    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Phase 3: Packet monitor test */
    test_packet_monitor();

    /* Done */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "════════════════════════════════════════");
    ESP_LOGI(TAG, "  STREAMING TEST COMPLETE");
    ESP_LOGI(TAG, "════════════════════════════════════════");

    while (1) {
        ESP_LOGI(TAG, "Free heap: %lu", (unsigned long)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
