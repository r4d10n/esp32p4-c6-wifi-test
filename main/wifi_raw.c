/*
 * WiFi Raw - Host-side implementation
 *
 * Sends commands to ESP32-C6 slave via CustomRpc and receives
 * promiscuous packet events and command responses.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_hosted_misc.h"
#include "wifi_raw.h"
#include "wifi_raw_msgs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_raw";

/* ─── Response synchronization ─── */
static EventGroupHandle_t s_resp_event;
#define RESP_RECEIVED_BIT  BIT0

static wifi_raw_cmd_response_t s_last_response;
static wifi_raw_rx_cb_t s_rx_cb = NULL;

/* ─── CustomRpc Callbacks ─── */

static void on_cmd_response(uint32_t msg_id, const uint8_t *data, size_t data_len)
{
    if (data_len >= sizeof(wifi_raw_cmd_response_t)) {
        memcpy(&s_last_response, data, sizeof(wifi_raw_cmd_response_t));
        xEventGroupSetBits(s_resp_event, RESP_RECEIVED_BIT);
    }
}

static void on_promisc_pkt(uint32_t msg_id, const uint8_t *data, size_t data_len)
{
    if (!s_rx_cb || data_len < sizeof(wifi_raw_promisc_pkt_t)) {
        return;
    }

    const wifi_raw_promisc_pkt_t *pkt = (const wifi_raw_promisc_pkt_t *)data;

    if (data_len < sizeof(wifi_raw_promisc_pkt_t) + pkt->data_len) {
        return;
    }

    wifi_raw_rx_pkt_t rx = {
        .type = pkt->type,
        .rssi = pkt->rssi,
        .channel = pkt->channel,
        .rate = pkt->rate,
        .sig_mode = pkt->sig_mode,
        .rx_state = pkt->rx_state,
        .payload = pkt->data,
        .payload_len = pkt->data_len,
    };

    s_rx_cb(&rx);
}

/* ─── Wait for command response ─── */

static esp_err_t wait_cmd_response(uint16_t expected_cmd, TickType_t timeout)
{
    EventBits_t bits = xEventGroupWaitBits(s_resp_event, RESP_RECEIVED_BIT,
                                            pdTRUE, pdTRUE, timeout);
    if (!(bits & RESP_RECEIVED_BIT)) {
        ESP_LOGE(TAG, "Command 0x%04x: timeout", expected_cmd);
        return ESP_ERR_TIMEOUT;
    }

    if (s_last_response.cmd_msg_id != expected_cmd) {
        ESP_LOGW(TAG, "Response mismatch: expected 0x%04x got 0x%04x",
                 expected_cmd, s_last_response.cmd_msg_id);
    }

    return (esp_err_t)s_last_response.status;
}

/* ─── Public API ─── */

esp_err_t wifi_raw_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi raw packet system");

    s_resp_event = xEventGroupCreate();
    if (!s_resp_event) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret;

    ret = esp_hosted_register_custom_callback(WIFI_RAW_MSG_CMD_RESPONSE, on_cmd_response);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register CMD_RESPONSE callback: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_hosted_register_custom_callback(WIFI_RAW_MSG_PROMISC_PKT, on_promisc_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PROMISC_PKT callback: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi raw packet system ready");
    return ESP_OK;
}

esp_err_t wifi_raw_set_promiscuous(bool enable)
{
    wifi_raw_cmd_set_promiscuous_t cmd = { .enable = enable ? 1 : 0 };

    xEventGroupClearBits(s_resp_event, RESP_RECEIVED_BIT);
    esp_err_t ret = esp_hosted_send_custom_data(WIFI_RAW_MSG_SET_PROMISCUOUS,
                                                 (const uint8_t *)&cmd, sizeof(cmd));
    if (ret != ESP_OK) return ret;

    return wait_cmd_response(WIFI_RAW_MSG_SET_PROMISCUOUS, pdMS_TO_TICKS(5000));
}

esp_err_t wifi_raw_set_channel(uint8_t primary, uint8_t second)
{
    wifi_raw_cmd_set_channel_t cmd = { .primary = primary, .second = second };

    xEventGroupClearBits(s_resp_event, RESP_RECEIVED_BIT);
    esp_err_t ret = esp_hosted_send_custom_data(WIFI_RAW_MSG_SET_CHANNEL,
                                                 (const uint8_t *)&cmd, sizeof(cmd));
    if (ret != ESP_OK) return ret;

    return wait_cmd_response(WIFI_RAW_MSG_SET_CHANNEL, pdMS_TO_TICKS(5000));
}

esp_err_t wifi_raw_set_filter(uint32_t filter_mask)
{
    wifi_raw_cmd_set_filter_t cmd = { .filter_mask = filter_mask };

    xEventGroupClearBits(s_resp_event, RESP_RECEIVED_BIT);
    esp_err_t ret = esp_hosted_send_custom_data(WIFI_RAW_MSG_SET_FILTER,
                                                 (const uint8_t *)&cmd, sizeof(cmd));
    if (ret != ESP_OK) return ret;

    return wait_cmd_response(WIFI_RAW_MSG_SET_FILTER, pdMS_TO_TICKS(5000));
}

esp_err_t wifi_raw_80211_tx(uint8_t ifx, const void *buffer, int len, bool en_sys_seq)
{
    if (!buffer || len <= 0 || len > 4000) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t cmd_size = sizeof(wifi_raw_cmd_80211_tx_t) + len;
    uint8_t *cmd_buf = malloc(cmd_size);
    if (!cmd_buf) {
        return ESP_ERR_NO_MEM;
    }

    wifi_raw_cmd_80211_tx_t *cmd = (wifi_raw_cmd_80211_tx_t *)cmd_buf;
    cmd->ifx = ifx;
    cmd->en_sys_seq = en_sys_seq ? 1 : 0;
    cmd->data_len = (uint16_t)len;
    memcpy(cmd->data, buffer, len);

    xEventGroupClearBits(s_resp_event, RESP_RECEIVED_BIT);
    esp_err_t ret = esp_hosted_send_custom_data(WIFI_RAW_MSG_80211_TX, cmd_buf, cmd_size);
    free(cmd_buf);

    if (ret != ESP_OK) return ret;

    return wait_cmd_response(WIFI_RAW_MSG_80211_TX, pdMS_TO_TICKS(5000));
}

void wifi_raw_register_rx_cb(wifi_raw_rx_cb_t cb)
{
    s_rx_cb = cb;
}
