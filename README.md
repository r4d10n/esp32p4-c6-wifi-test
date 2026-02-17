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

## Transport & Throughput Analysis

### P4 ↔ C6 Communication

The P4 and C6 are connected via **SDIO only** on the Waveshare board (7 wires: CLK, CMD, D0-D3, RESET GPIO 54). No SPI, UART, or other bus is available.

| Transport | Max Throughput | Status |
|-----------|---------------|--------|
| **SDIO 4-bit @ 40 MHz** | **79.5 Mbps** | **Wired on board, in use** |
| SPI Full-Duplex @ 40 MHz | 25 Mbps | Not wired |
| SPI Half-Duplex Quad | 50-80 Mbps (est.) | Not wired |
| UART @ 921600 baud | 0.74 Mbps | Not wired |

SDIO is ~3x faster than SPI-FD at the same clock. The WiFi PHY (~36 Mbps) is the bottleneck, not the transport (79.5 Mbps) — there is **2.5x headroom** on SDIO.

### Why WiFi 6 Throughput is "Only" 36 Mbps

The ESP32-C6 supports 802.11ax (WiFi 6) with a PHY maximum of 143.4 Mbps (HE MCS11, HT20, GI=0.8µs). Real-world throughput is 36 Mbps — **25.1% PHY efficiency**. The gap is explained by four cascading factors:

| Stage | Mbps | Loss | Cause |
|-------|------|------|-------|
| PHY max (HE MCS11, HT20) | 143.4 | — | Spec ceiling |
| Realistic MCS7 (64-QAM 5/6) | 86.0 | -57.4 | MCS11 needs ~-40 dBm; typical indoor RSSI is -55 to -65 dBm |
| After MAC overhead (A-MPDU, BA win=6) | 61.9 | -24.1 | 72% MAC efficiency — SIFS/DIFS/ACK/BA handshake |
| **Single-core CPU bottleneck** | **46.5** | **-15.4** | **Primary architectural limit** (1×160 MHz RISC-V) |
| lwIP + socket + SDIO | 42.0 | -4.5 | Protocol stack overhead |
| **Measured application** | **~36** | -6.0 | Real-world variance |

**The C6's single-core 160 MHz RISC-V is the primary bottleneck.** At 36 Mbps / 1400B packets = 3,214 pps, each packet gets ~49,800 CPU cycles to traverse: WiFi DMA ISR → lwIP → SDIO DMA. The ESP32-C5 (dual-core 240 MHz, same HE radio) achieves ~55 Mbps.

#### WiFi 6 Features on C6

| Feature | Status | Throughput Impact |
|---------|--------|-------------------|
| 1024-QAM (MCS10/11) | Yes (PHY capable) | Rarely used — needs excellent SNR |
| MU-MIMO | **No** — 1T1R single antenna | Major speed gains come from this |
| 5 GHz band | **No** — 2.4 GHz only | No access to wider/cleaner channels |
| OFDMA (DL/UL) | Partial (STA-side) | Minor improvement |
| BSS Coloring | Yes | Reduces congestion collisions |
| TWT | Yes | Power saving, not throughput |
| A-MPDU / A-MSDU | Yes | Enabled, BA window=6 (low) |

The "WiFi 6" label is accurate but misleading. The big speed gains (MU-MIMO, 5 GHz) are absent. The C6's WiFi 6 advantage is better modulation coding and interference management, not raw speed.

#### Cross-Chip Comparison

| Chip | CPU | WiFi | Measured UDP | PHY Efficiency |
|------|-----|------|-------------|----------------|
| ESP32 | 2×240 MHz Xtensa | 802.11n | ~20 Mbps | 27.7% |
| ESP32-S3 | 2×240 MHz Xtensa | 802.11n | ~25 Mbps | 34.6% |
| **ESP32-C6** | **1×160 MHz RISC-V** | **802.11ax** | **~36 Mbps** | **25.1%** |
| ESP32-C5 | 2×240 MHz RISC-V | 802.11ax | ~55 Mbps | 38.4% |

### esp-hosted RPC Overhead

WiFi data frames and RPC commands share a **single SDIO Function 1 channel**. There is no hardware separation — multiplexing is purely software via `if_type` in a 12-byte `esp_payload_header`.

| Overhead Source | Cost | Impact |
|----------------|------|--------|
| Per-packet header | 12 bytes | 0.79% at 1500B MTU |
| SDIO block alignment | 24 bytes padding (at 1500B) | 2.3% wire overhead |
| RPC priority preemption | 25.6 µs per RPC event | WiFi frames deferred during RPC |
| RPC round-trip latency | ~0.5 ms | Limits raw TX to ~2000 frames/sec |
| Promiscuous mode forwarding | ~85 µs CPU per packet | 8.5% C6 CPU at 1000 pkt/s |
| SDIO RX buffer pool | 60 KB (40 × 1536B) | 11.7% of C6's 512 KB SRAM |

**Key insight**: RPC commands have the highest TX/RX priority (`PRIO_Q_SERIAL`), dequeued before WiFi data (`PRIO_Q_OTHERS`). CustomRpc (promiscuous/raw TX) uses the same priority path — high promiscuous capture rates directly compete with WiFi data throughput.

For normal WiFi STA data (no promiscuous mode), esp-hosted adds only ~3% wire overhead. The SDIO transport at 40 MHz (160 Mbps effective) has ample headroom for the WiFi PHY ceiling.

### Optimization Opportunities

Quick-win `sdkconfig.defaults` changes, ordered by expected impact:

```ini
# 1. TCP window size — CRITICAL (current 5760B caps TCP at ~15 Mbps)
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535
CONFIG_LWIP_TCP_WND_DEFAULT=65535
CONFIG_LWIP_TCP_RECVMBOX_SIZE=32

# 2. A-MPDU BA window (current=6, max=32 for HT / 256 for HE)
CONFIG_ESP_WIFI_TX_BA_WIN=32
CONFIG_ESP_WIFI_RX_BA_WIN=32

# 3. lwIP IRAM placement (reduces per-packet latency)
CONFIG_LWIP_IRAM_OPTIMIZATION=y

# 4. SDIO clock bump (test signal integrity first)
CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=50000

# 5. Larger SDIO queues
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=64
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=64

# 6. Pin lwIP to core 1 (P4 is dual-core)
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y
```

**Already optimal**: WiFi power save disabled (`WIFI_PS_NONE`), system PM disabled, network buffers in internal SRAM (not PSRAM), mempool 64-byte aligned, SDIO streaming mode, WiFi/GDMA in IRAM.

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
