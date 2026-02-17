/*
 * ESP32-P4 WiFi Test Firmware
 *
 * Tests WiFi connectivity through esp-hosted (ESP32-C6 co-processor over SDIO).
 * Probes raw 802.11 TX/RX APIs and falls back to UDP throughput test.
 *
 * Test sequence:
 *   Phase 1: WiFi initialization via esp-hosted
 *   Phase 2: API capability probe (which esp_wifi functions work)
 *   Phase 3: Raw 802.11 TX test (if available)
 *   Phase 4: Promiscuous RX test (if available)
 *   Phase 5: UDP broadcast throughput test (fallback)
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "wifi_test";

/* ─── Weak stubs for esp-hosted functions that may not be implemented ───
 * esp-hosted v0.0.27 has these in its protobuf schema (RPC IDs 305-317)
 * but the host-side C wrappers are commented out in esp_hosted_api.c.
 * Providing weak fallbacks lets us probe at runtime without linker errors. */
__attribute__((weak)) esp_err_t esp_wifi_80211_tx(wifi_interface_t ifx,
    const void *buffer, int len, bool en_sys_seq)
{
    (void)ifx; (void)buffer; (void)len; (void)en_sys_seq;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t esp_wifi_set_promiscuous(bool en)
{
    (void)en;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *filter)
{
    (void)filter;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t esp_wifi_config_80211_tx_rate(wifi_interface_t ifx,
    wifi_phy_rate_t rate)
{
    (void)ifx; (void)rate;
    return ESP_ERR_NOT_SUPPORTED;
}

/* ─── Test Configuration ─── */
#define TEST_WIFI_CHANNEL       7
#define TEST_DURATION_SEC       10
#define TX_PACKET_SIZE          1400
#define STATS_INTERVAL_MS       1000
#define UDP_BROADCAST_PORT      5000
#define UDP_BROADCAST_IP        "255.255.255.255"

/* ─── Counters ─── */
static volatile uint32_t s_tx_packets = 0;
static volatile uint32_t s_tx_bytes = 0;
static volatile uint32_t s_tx_errors = 0;
static volatile bool s_tx_running = false;

static void reset_counters(void)
{
    s_tx_packets = 0;
    s_tx_bytes = 0;
    s_tx_errors = 0;
}

static void print_tx_stats(const char *label, int elapsed_sec)
{
    uint32_t pkts = s_tx_packets;
    uint32_t bytes = s_tx_bytes;
    uint32_t errs = s_tx_errors;
    float mbps = (elapsed_sec > 0) ? (bytes * 8.0f / 1000000.0f / elapsed_sec) : 0;
    float pps = (elapsed_sec > 0) ? ((float)pkts / elapsed_sec) : 0;

    ESP_LOGI(TAG, "│ %s [%ds]: %lu pkts (%.0f pps) | %.2f Mbps | err:%lu",
             label, elapsed_sec,
             (unsigned long)pkts, pps, mbps, (unsigned long)errs);
}

/* ─── Phase 1: WiFi Init ─── */
static esp_err_t wifi_init(void)
{
    ESP_LOGI(TAG, "════════════════════════════════════════");
    ESP_LOGI(TAG, "  Phase 1: WiFi Initialization");
    ESP_LOGI(TAG, "════════════════════════════════════════");

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[OK] NVS initialized");

    /* Network stack (esp-hosted may have already created the event loop) */
    ESP_ERROR_CHECK(esp_netif_init());
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    ESP_LOGI(TAG, "[OK] Network stack initialized");

    /* Create AP netif (esp-hosted may have already created it) */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGW(TAG, "[SKIP] AP netif already exists (created by esp-hosted)");
    } else {
        ESP_LOGI(TAG, "[OK] AP netif created");
    }

    /* WiFi init via esp_wifi_remote -> esp-hosted -> C6 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] esp_wifi_init: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGE(TAG, "  Check: C6 slave firmware, SDIO wiring");
        return ret;
    }
    ESP_LOGI(TAG, "[OK] esp_wifi_init succeeded (esp-hosted SDIO connected!)");

    /* Set AP mode - hidden, no clients */
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32P4-FPV-TEST",
            .ssid_len = 16,
            .channel = TEST_WIFI_CHANNEL,
            .max_connection = 0,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 1,
        },
    };
    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] esp_wifi_set_mode(AP): %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[OK] WiFi mode set to AP");

    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] esp_wifi_set_config: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Disable power saving */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Start WiFi */
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] esp_wifi_start: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }
    ESP_LOGI(TAG, "[OK] WiFi started on channel %d", TEST_WIFI_CHANNEL);

    /* Read back MAC */
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        ESP_LOGI(TAG, "[OK] AP MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    ESP_LOGI(TAG, "Phase 1 COMPLETE");
    return ESP_OK;
}

/* ─── Phase 2: API Capability Probe ─── */
typedef struct {
    bool set_channel;
    bool set_bandwidth;
    bool set_protocol;
    bool set_max_tx_power;
    bool raw_80211_tx;
    bool set_promiscuous;
    bool set_promiscuous_filter;
    bool config_80211_tx_rate;
} wifi_caps_t;

static wifi_caps_t probe_wifi_capabilities(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "════════════════════════════════════════");
    ESP_LOGI(TAG, "  Phase 2: API Capability Probe");
    ESP_LOGI(TAG, "════════════════════════════════════════");

    wifi_caps_t caps = {0};
    esp_err_t ret;

    /* Basic WiFi control APIs */
    ret = esp_wifi_set_channel(TEST_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    caps.set_channel = (ret == ESP_OK);
    ESP_LOGI(TAG, "  esp_wifi_set_channel:       %s (%s)",
             caps.set_channel ? "OK" : "FAIL", esp_err_to_name(ret));

    ret = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    caps.set_bandwidth = (ret == ESP_OK);
    ESP_LOGI(TAG, "  esp_wifi_set_bandwidth:     %s (%s)",
             caps.set_bandwidth ? "OK" : "FAIL", esp_err_to_name(ret));

    ret = esp_wifi_set_protocol(WIFI_IF_AP,
            WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    caps.set_protocol = (ret == ESP_OK);
    ESP_LOGI(TAG, "  esp_wifi_set_protocol:      %s (%s)",
             caps.set_protocol ? "OK" : "FAIL", esp_err_to_name(ret));

    ret = esp_wifi_set_max_tx_power(80);
    caps.set_max_tx_power = (ret == ESP_OK);
    ESP_LOGI(TAG, "  esp_wifi_set_max_tx_power:  %s (%s)",
             caps.set_max_tx_power ? "OK" : "FAIL", esp_err_to_name(ret));

    /* Raw 802.11 TX - this is what FPV needs */
    uint8_t test_frame[64] = {
        0x08, 0x00, 0x00, 0x00,                         /* Data frame */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,             /* Broadcast */
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,             /* Src MAC */
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,             /* BSSID */
        0x00, 0x00,                                      /* Seq control */
    };
    memset(test_frame + 24, 0xAA, 40);  /* Payload */

    ret = esp_wifi_80211_tx(WIFI_IF_AP, test_frame, sizeof(test_frame), false);
    caps.raw_80211_tx = (ret == ESP_OK);
    ESP_LOGI(TAG, "  esp_wifi_80211_tx:          %s (%s)",
             caps.raw_80211_tx ? "OK" : "FAIL", esp_err_to_name(ret));

    /* Promiscuous mode */
    ret = esp_wifi_set_promiscuous(true);
    caps.set_promiscuous = (ret == ESP_OK);
    ESP_LOGI(TAG, "  esp_wifi_set_promiscuous:   %s (%s)",
             caps.set_promiscuous ? "OK" : "FAIL", esp_err_to_name(ret));
    if (caps.set_promiscuous) {
        esp_wifi_set_promiscuous(false);  /* Turn off for now */
    }

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA,
    };
    ret = esp_wifi_set_promiscuous_filter(&filter);
    caps.set_promiscuous_filter = (ret == ESP_OK);
    ESP_LOGI(TAG, "  esp_wifi_set_promiscuous_filter: %s (%s)",
             caps.set_promiscuous_filter ? "OK" : "FAIL", esp_err_to_name(ret));

    /* PHY rate configuration */
    ret = esp_wifi_config_80211_tx_rate(WIFI_IF_AP, WIFI_PHY_RATE_MCS3_LGI);
    caps.config_80211_tx_rate = (ret == ESP_OK);
    ESP_LOGI(TAG, "  esp_wifi_config_80211_tx_rate: %s (%s)",
             caps.config_80211_tx_rate ? "OK" : "FAIL", esp_err_to_name(ret));

    /* Summary */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "┌─── Capability Summary ───────────────");
    ESP_LOGI(TAG, "│ Basic WiFi control: %s",
             (caps.set_channel && caps.set_bandwidth) ? "YES" : "PARTIAL");
    ESP_LOGI(TAG, "│ Raw 802.11 TX:      %s",
             caps.raw_80211_tx ? "YES - FPV injection possible!" : "NO - need UDP fallback");
    ESP_LOGI(TAG, "│ Promiscuous RX:     %s",
             caps.set_promiscuous ? "YES - monitor mode available!" : "NO");
    ESP_LOGI(TAG, "│ PHY rate control:   %s",
             caps.config_80211_tx_rate ? "YES" : "NO");
    ESP_LOGI(TAG, "└──────────────────────────────────────");

    return caps;
}

/* ─── Phase 3: Raw 802.11 TX Throughput ─── */
static void test_raw_tx_throughput(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "════════════════════════════════════════");
    ESP_LOGI(TAG, "  Phase 3: Raw 802.11 TX Throughput (%ds)", TEST_DURATION_SEC);
    ESP_LOGI(TAG, "════════════════════════════════════════");

    /* Build packet: IEEE header + payload */
    const uint8_t ieee_hdr[24] = {
        0x08, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x00, 0x00,
    };
    const size_t total = 24 + TX_PACKET_SIZE;
    uint8_t *pkt = malloc(total);
    if (!pkt) {
        ESP_LOGE(TAG, "malloc failed");
        return;
    }
    memcpy(pkt, ieee_hdr, 24);
    for (int i = 0; i < TX_PACKET_SIZE; i++)
        pkt[24 + i] = (uint8_t)(i & 0xFF);

    reset_counters();
    int64_t start = esp_timer_get_time();

    for (int sec = 1; sec <= TEST_DURATION_SEC; sec++) {
        int64_t deadline = start + (int64_t)sec * 1000000;
        while (esp_timer_get_time() < deadline) {
            esp_err_t ret = esp_wifi_80211_tx(WIFI_IF_AP, pkt, total, false);
            if (ret == ESP_OK) {
                s_tx_packets++;
                s_tx_bytes += total;
            } else if (ret == ESP_ERR_NO_MEM) {
                taskYIELD();
            } else {
                s_tx_errors++;
            }
        }
        print_tx_stats("RAW TX", sec);
    }

    float elapsed = (esp_timer_get_time() - start) / 1000000.0f;
    float mbps = s_tx_bytes * 8.0f / 1000000.0f / elapsed;

    ESP_LOGI(TAG, "╔═══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  RAW TX: %.2f Mbps (%lu pkts, %lu err) ║",
             mbps, (unsigned long)s_tx_packets, (unsigned long)s_tx_errors);
    ESP_LOGI(TAG, "╚═══════════════════════════════════════╝");

    free(pkt);
}

/* ─── Phase 4: UDP Broadcast TX Throughput ─── */
static void udp_tx_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_BROADCAST_PORT),
    };
    inet_aton(UDP_BROADCAST_IP, &dest.sin_addr);

    uint8_t *buf = malloc(TX_PACKET_SIZE);
    if (!buf) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    for (int i = 0; i < TX_PACKET_SIZE; i++)
        buf[i] = (uint8_t)(i & 0xFF);

    ESP_LOGI(TAG, "UDP TX task started (port %d, %d byte payloads)",
             UDP_BROADCAST_PORT, TX_PACKET_SIZE);

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
    ESP_LOGI(TAG, "UDP TX task stopped");
    vTaskDelete(NULL);
}

static void test_udp_throughput(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "════════════════════════════════════════");
    ESP_LOGI(TAG, "  Phase 4: UDP Broadcast TX Throughput (%ds)", TEST_DURATION_SEC);
    ESP_LOGI(TAG, "════════════════════════════════════════");

    reset_counters();
    s_tx_running = true;

    xTaskCreatePinnedToCore(udp_tx_task, "udp_tx", 4096, NULL,
                            configMAX_PRIORITIES - 2, NULL, 0);

    for (int sec = 1; sec <= TEST_DURATION_SEC; sec++) {
        vTaskDelay(pdMS_TO_TICKS(STATS_INTERVAL_MS));
        print_tx_stats("UDP TX", sec);
    }

    s_tx_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));

    float mbps = s_tx_bytes * 8.0f / 1000000.0f / TEST_DURATION_SEC;

    ESP_LOGI(TAG, "╔═══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  UDP TX: %.2f Mbps (%lu pkts, %lu err) ║",
             mbps, (unsigned long)s_tx_packets, (unsigned long)s_tx_errors);
    ESP_LOGI(TAG, "╚═══════════════════════════════════════╝");
}

/* ─── Main ─── */
void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ESP32-P4 WiFi Test via esp-hosted (ESP32-C6)    ║");
    ESP_LOGI(TAG, "║  API Probe + Throughput Measurement              ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    /* Phase 1: Initialize WiFi */
    esp_err_t ret = wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "═════════════════════════════════════════════");
        ESP_LOGE(TAG, "  FATAL: WiFi initialization failed!");
        ESP_LOGE(TAG, "  Possible causes:");
        ESP_LOGE(TAG, "    1. ESP32-C6 has no slave firmware");
        ESP_LOGE(TAG, "    2. SDIO wiring problem");
        ESP_LOGE(TAG, "    3. ESP-Hosted version mismatch");
        ESP_LOGE(TAG, "═════════════════════════════════════════════");

        /* Retry loop */
        for (int i = 1; i <= 3; i++) {
            ESP_LOGI(TAG, "Retry %d/3 in 5 seconds...", i);
            vTaskDelay(pdMS_TO_TICKS(5000));
            ret = wifi_init();
            if (ret == ESP_OK) break;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "All retries failed. Halting.");
            while (1) vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Phase 2: Probe capabilities */
    wifi_caps_t caps = probe_wifi_capabilities();
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Phase 3: Raw 802.11 TX (if available) */
    if (caps.raw_80211_tx) {
        test_raw_tx_throughput();
        vTaskDelay(pdMS_TO_TICKS(2000));
    } else {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "Skipping raw 802.11 TX test (not available via esp-hosted)");
    }

    /* Phase 4: UDP throughput (always available as baseline) */
    test_udp_throughput();

    /* Done */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "════════════════════════════════════════");
    ESP_LOGI(TAG, "  ALL TESTS COMPLETE");
    ESP_LOGI(TAG, "════════════════════════════════════════");

    /* Idle loop */
    while (1) {
        ESP_LOGI(TAG, "Free heap: %lu (internal: %lu)",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
