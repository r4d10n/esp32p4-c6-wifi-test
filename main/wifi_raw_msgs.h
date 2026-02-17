/*
 * WiFi Raw Packet Injection/Monitoring Protocol
 *
 * Shared message definitions for CustomRpc communication between
 * ESP32-P4 host and ESP32-C6 slave via esp-hosted.
 */

#ifndef WIFI_RAW_MSGS_H
#define WIFI_RAW_MSGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Command Message IDs (Host → Slave) ─── */
#define WIFI_RAW_MSG_SET_PROMISCUOUS    0x0100
#define WIFI_RAW_MSG_SET_CHANNEL        0x0101
#define WIFI_RAW_MSG_SET_FILTER         0x0102
#define WIFI_RAW_MSG_80211_TX           0x0103

/* ─── Response/Event Message IDs (Slave → Host) ─── */
#define WIFI_RAW_MSG_CMD_RESPONSE       0x0180
#define WIFI_RAW_MSG_PROMISC_PKT        0x0200

/* ─── Command Payloads (Host → Slave) ─── */

typedef struct {
    uint8_t enable;         /* 1 = enable, 0 = disable */
} __attribute__((packed)) wifi_raw_cmd_set_promiscuous_t;

typedef struct {
    uint8_t primary;        /* Primary channel (1-14) */
    uint8_t second;         /* Secondary channel: 0=none, 1=above, 2=below */
} __attribute__((packed)) wifi_raw_cmd_set_channel_t;

typedef struct {
    uint32_t filter_mask;   /* wifi_promiscuous_filter_t filter bits */
} __attribute__((packed)) wifi_raw_cmd_set_filter_t;

typedef struct {
    uint8_t ifx;            /* wifi_interface_t: 0=STA, 1=AP */
    uint8_t en_sys_seq;     /* 1 = let driver set sequence number */
    uint16_t data_len;      /* Length of 802.11 frame data */
    uint8_t data[];         /* Raw 802.11 frame (flexible array) */
} __attribute__((packed)) wifi_raw_cmd_80211_tx_t;

/* ─── Response/Event Payloads (Slave → Host) ─── */

typedef struct {
    uint16_t cmd_msg_id;    /* Original command message ID */
    int32_t status;         /* esp_err_t result */
} __attribute__((packed)) wifi_raw_cmd_response_t;

typedef struct {
    uint32_t type;          /* wifi_promiscuous_pkt_type_t */
    int8_t rssi;            /* Signal strength */
    uint8_t channel;        /* Channel packet was received on */
    uint8_t rate;           /* Data rate */
    uint8_t sig_mode;       /* 0=non-HT, 1=HT, 3=VHT */
    uint32_t rx_state;      /* RX state (0 = no error) */
    uint16_t data_len;      /* Length of 802.11 frame */
    uint8_t data[];         /* Raw 802.11 frame (flexible array) */
} __attribute__((packed)) wifi_raw_promisc_pkt_t;

#ifdef __cplusplus
}
#endif

#endif /* WIFI_RAW_MSGS_H */
