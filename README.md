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

### API Capability Probe

| API | Status |
|-----|--------|
| `esp_wifi_set_channel` | OK |
| `esp_wifi_set_bandwidth` | OK |
| `esp_wifi_set_protocol` | OK |
| `esp_wifi_set_max_tx_power` | OK |
| `esp_wifi_80211_tx` | NOT SUPPORTED |
| `esp_wifi_set_promiscuous` | NOT SUPPORTED |
| `esp_wifi_config_80211_tx_rate` | NOT SUPPORTED |

Raw 802.11 TX and promiscuous mode are not exposed through esp-hosted's host-side API (RPC stubs exist but are commented out).

## Projects

### WiFi Streaming Test (`main/`)

Connects as WiFi STA via esp-hosted, streams UDP to a target host, and measures throughput.

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
