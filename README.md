# ESP32-P4 + C6 WiFi Test

WiFi connectivity and throughput testing for ESP32-P4 with ESP32-C6 co-processor over SDIO, using [esp-hosted](https://github.com/espressif/esp-hosted).

Tested on **Waveshare ESP32-P4-WIFI6-NANO**.

## Results

### Throughput Progression

| Config | SDIO Clock | Bandwidth | Sender (Mbps) | Receiver (Mbps) | Notes |
|--------|-----------|-----------|---------------|-----------------|-------|
| Baseline | 25 MHz | HT20 | 33.46 | 29-33 | ~9.6% packet loss |
| Faster SDIO | 40 MHz | HT20 | 40.14 | ~36 | SDIO was the bottleneck |
| Wider channel | 40 MHz | HT40 | 42.56 | 13-25 (degrading) | 2.4 GHz congestion, worse |

**Best config: 40 MHz SDIO + HT20** — ~36 Mbps real-world throughput.

HT40 increased sender throughput slightly but caused severe packet loss due to 2.4 GHz interference. The wider 40 MHz channel overlaps with neighboring networks.

### Legacy Tests

| Test | Throughput | Notes |
|------|-----------|-------|
| UDP broadcast (AP mode, loopback) | 33.61 Mbps | Zero errors, 3001 pps |

### Packet Monitoring (Promiscuous Mode)

Promiscuous mode captures raw 802.11 frames on the C6 and forwards them to the P4 over SDIO via esp-hosted CustomRpc.

| Metric | Value |
|--------|-------|
| Packets captured | 514 in 10s |
| Management frames | 190 (beacons, auth, deauth, probes) |
| Data frames | 324 |
| Control frames | 0 (filtered) |

### API Support

| API | Direct (esp-hosted RPC) | Via CustomRpc Extension |
|-----|------------------------|------------------------|
| `esp_wifi_set_channel` | OK | OK |
| `esp_wifi_set_bandwidth` | OK | - |
| `esp_wifi_set_protocol` | OK | - |
| `esp_wifi_set_max_tx_power` | OK | - |
| `esp_wifi_80211_tx` | NOT SUPPORTED | **OK** |
| `esp_wifi_set_promiscuous` | NOT SUPPORTED | **OK** |
| `esp_wifi_set_promiscuous_filter` | NOT SUPPORTED | **OK** |
| `esp_wifi_config_80211_tx_rate` | NOT SUPPORTED | - |

Raw 802.11 TX and promiscuous mode are not exposed through esp-hosted's built-in RPC layer (stubs exist but are commented out). These are implemented via a **CustomRpc extension** that adds command handlers to the C6 slave firmware and a host-side library on the P4.

## WiFi Raw Packet Extension

### Architecture

```
ESP32-P4 (Host)                    ESP32-C6 (Slave)
┌──────────────────┐               ┌──────────────────┐
│  wifi_raw.c/h    │               │ wifi_raw_slave.c  │
│  (Host API)      │               │ (Command handlers)│
│                  │   CustomRpc   │                   │
│  Commands ───────┼──── SDIO ────►│  esp_wifi_*()     │
│                  │               │  native APIs      │
│  RX callback ◄───┼──── SDIO ────┤                   │
│  (promisc pkts)  │   (events)   │  promisc_rx_cb()  │
└──────────────────┘               └──────────────────┘
```

The extension uses esp-hosted's CustomRpc mechanism (RPC ID 388, available since v2.8.1) for bidirectional communication without modifying esp-hosted source code.

### Protocol

Shared message definitions in `wifi_raw_msgs.h`:

| Message ID | Direction | Description |
|-----------|-----------|-------------|
| `0x0100` | Host -> Slave | Set promiscuous mode (enable/disable) |
| `0x0101` | Host -> Slave | Set WiFi channel |
| `0x0102` | Host -> Slave | Set promiscuous filter mask |
| `0x0103` | Host -> Slave | Transmit raw 802.11 frame |
| `0x0180` | Slave -> Host | Command response (status) |
| `0x0200` | Slave -> Host | Captured promiscuous packet |

All payloads use `__attribute__((packed))` structs for wire compatibility between the RISC-V P4 and C6.

### Host API (`wifi_raw.h`)

```c
#include "wifi_raw.h"

// Initialize (registers CustomRpc callbacks)
wifi_raw_init();

// Enable promiscuous mode
wifi_raw_set_promiscuous(true);

// Set channel (primary=6, no secondary)
wifi_raw_set_channel(6, 0);

// Set filter (management + data frames)
wifi_raw_set_filter(WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA);

// Register RX callback for captured packets
wifi_raw_register_rx_cb(my_callback);

// Inject a raw 802.11 frame
wifi_raw_80211_tx(WIFI_IF_STA, frame_buffer, frame_len, true);
```

All commands are synchronous with a 5-second timeout. The RX callback receives `wifi_raw_rx_pkt_t` with RSSI, channel, rate, signal mode, and the raw frame payload.

### Slave Extension (`wifi_raw_slave.c`)

The slave registers 4 command handlers on init that call ESP-IDF native WiFi APIs:
- `esp_wifi_set_promiscuous()` / `esp_wifi_set_promiscuous_rx_cb()`
- `esp_wifi_set_channel()`
- `esp_wifi_set_promiscuous_filter()`
- `esp_wifi_80211_tx()`

Captured promiscuous frames are forwarded to the host via CustomRpc events. Frame length is capped at 4000 bytes.

### Building the Custom Slave

The slave firmware must be rebuilt with the wifi_raw extension and re-flashed to the C6 via OTA:

```bash
# Build custom slave
source ~/idf.sh
idf.py -C slave set-target esp32c6
idf.py -C slave build

# Copy to OTA flasher
cp slave/build/network_adapter.bin c6-ota-flasher/

# Flash OTA flasher to P4 (no monitor - serial interferes with OTA)
cd c6-ota-flasher
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash

# Wait ~45s for OTA to complete, then flash the test app
cd ..
idf.py -p /dev/ttyACM0 flash monitor
```

### Configuration

Both host and slave `sdkconfig.defaults` require:
```
CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y
CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS=8
```

### Supported Frame Types for Injection

Per ESP-IDF `esp_wifi_80211_tx()`: beacon, probe request, probe response, action, and non-QoS data frames. QoS data frames and Block ACK are not supported.

### Limitations

- **Single-threaded command API**: The host-side command/response uses a shared EventGroup. Do not call `wifi_raw_*` commands from multiple FreeRTOS tasks concurrently without adding a mutex.
- **Channel-locked monitoring**: Promiscuous mode captures on the current channel only. Changing channels while STA is connected will disrupt the connection.
- **Throughput impact**: Enabling promiscuous mode reduces STA throughput.
- **PHY rate control**: `esp_wifi_config_80211_tx_rate()` is not exposed via this extension (can be added if needed).

## Projects

### WiFi Test App (`main/`)

Three-phase test: WiFi STA connect, UDP throughput measurement, and packet monitoring via promiscuous mode.

```
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Configure WiFi credentials and target IP in `main/app_main.c`:
```c
#define WIFI_SSID_PRIMARY   "YourSSID"
#define WIFI_PASS           "YourPassword"
#define TARGET_IP           "192.168.1.100"
#define TARGET_PORT         5001
```

UDP receiver on host:
```bash
python3 -c "
import socket, time
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(('0.0.0.0', 5001))
total = 0; start = None
while True:
    data, _ = sock.recvfrom(2048)
    if not start: start = time.time()
    total += len(data)
    e = time.time() - start
    if int(e) > 0 and int(e) != int(e-0.01):
        print(f'[{int(e):2d}s] {total*8/1e6/e:.2f} Mbps')
"
```

### C6 OTA Flasher (`c6-ota-flasher/`)

Updates the ESP32-C6 slave firmware from the P4 over SDIO (no USB-UART adapter needed).

Embeds `network_adapter.bin` (esp-hosted slave v2.11.7) and pushes it using the esp-hosted OTA API.

```
cd c6-ota-flasher
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Key Learnings

- **WiFi init**: Use `WIFI_INIT_CONFIG_DEFAULT()` on P4 - zeroed config causes `ESP_ERR_INVALID_ARG`
- **Dependencies**: Need both `esp_wifi_remote` and `esp_hosted` in `idf_component.yml`
- **Kconfig**: Set `CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y` (not `ESP_WIFI_ENABLED`)
- **Event loop**: esp-hosted auto-creates it; handle `ESP_ERR_INVALID_STATE`
- **SDIO link**: Must call `esp_hosted_init()` + `esp_hosted_connect_to_slave()` for OTA
- **C6 factory firmware** (v0.0.0) must be updated to match host esp-hosted version

## Requirements

- ESP-IDF v5.4+
- ESP32-P4 board with ESP32-C6 co-processor (SDIO connection)
