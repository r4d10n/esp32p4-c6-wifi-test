/*
 * WiFi Raw - Host-side API for packet injection and monitoring
 *
 * Wraps esp-hosted CustomRpc to provide clean packet injection
 * and promiscuous mode monitoring from the ESP32-P4 host.
 * The actual WiFi operations execute on the ESP32-C6 slave.
 */

#ifndef WIFI_RAW_H
#define WIFI_RAW_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Promiscuous packet info passed to the RX callback
 */
typedef struct {
    uint32_t type;          /**< Packet type (wifi_promiscuous_pkt_type_t) */
    int8_t rssi;            /**< RSSI */
    uint8_t channel;        /**< Channel */
    uint8_t rate;           /**< Data rate */
    uint8_t sig_mode;       /**< 0=non-HT, 1=HT, 3=VHT */
    uint32_t rx_state;      /**< RX state (0 = no error) */
    const uint8_t *payload; /**< Raw 802.11 frame data */
    uint16_t payload_len;   /**< Frame length */
} wifi_raw_rx_pkt_t;

/**
 * @brief Callback for received promiscuous packets
 *
 * @param pkt Packet info (valid only during callback)
 */
typedef void (*wifi_raw_rx_cb_t)(const wifi_raw_rx_pkt_t *pkt);

/**
 * @brief Initialize the WiFi raw packet system
 *
 * Registers CustomRpc callbacks for command responses and
 * promiscuous packet events from the C6 slave.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_raw_init(void);

/**
 * @brief Enable or disable promiscuous (monitor) mode on C6
 *
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t wifi_raw_set_promiscuous(bool enable);

/**
 * @brief Set the WiFi channel for monitoring
 *
 * @param primary Primary channel (1-14)
 * @param second Secondary channel (0=none, 1=above, 2=below)
 * @return ESP_OK on success
 */
esp_err_t wifi_raw_set_channel(uint8_t primary, uint8_t second);

/**
 * @brief Set promiscuous mode packet filter
 *
 * @param filter_mask Bitmask of packet types to capture
 *                    (WIFI_PROMIS_FILTER_MASK_* values)
 * @return ESP_OK on success
 */
esp_err_t wifi_raw_set_filter(uint32_t filter_mask);

/**
 * @brief Transmit a raw 802.11 frame (packet injection)
 *
 * @param ifx Interface: 0=STA, 1=AP
 * @param buffer Raw 802.11 frame (including MAC header)
 * @param len Frame length
 * @param en_sys_seq If true, driver overwrites sequence number
 * @return ESP_OK on success
 */
esp_err_t wifi_raw_80211_tx(uint8_t ifx, const void *buffer, int len, bool en_sys_seq);

/**
 * @brief Register callback for promiscuous packets
 *
 * Only one callback can be active at a time. Pass NULL to deregister.
 *
 * @param cb Callback function
 */
void wifi_raw_register_rx_cb(wifi_raw_rx_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_RAW_H */
