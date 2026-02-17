# ESP32-P4 + C6 WiFi Throughput Test Results

## Hardware

```
+--------------------------------------------------+
|  Waveshare ESP32-P4-WIFI6-NANO                   |
|                                                    |
|  +-------------+    SDIO 4-bit    +-------------+ |
|  |  ESP32-P4   |<--- 40 MHz ---->|  ESP32-C6   | |
|  |  360 MHz    |    (~160 Mbps)   |  160 MHz    | |
|  |  32MB PSRAM |                  |  WiFi 6     | |
|  |  Host MCU   |                  |  2.4 GHz    | |
|  +-------------+                  +-------------+ |
|        |                               |           |
|     USB-C                         WiFi Antenna     |
|   /dev/ttyACM0                    2.4 GHz HT20    |
+--------------------------------------------------+
         |                               |
    [Dev Machine]                   [WiFi AP]
    Build & Flash              MALARnetJio_EXT (ch12)
                                     |
                              [Target Server]
                             192.168.1.128
                           iperf3 -s -p 5001/5002
```

- **ESP-IDF**: v5.5.1
- **esp-hosted**: v2.11.7
- **Transport**: SDIO 4-bit, 40 MHz clock
- **WiFi**: 2.4 GHz, HT20 (20 MHz bandwidth), channel 12
- **Test target**: 192.168.1.128 (UDP port 5001, TCP port 5002)

## Test Results Summary

### Final Configuration (Optimized Host, Factory Slave)

| Test | Throughput | Packets/s | Duration | Errors |
|------|-----------|-----------|----------|--------|
| **UDP TX** | **42.18 Mbps** | 3,766 pps | 30s sustained | 3,405 |
| **TCP TX** | **16.45 Mbps** | 125 pps | 30s sustained | 0 |
| **Promiscuous** | 216 pkts captured | - | 10s | - |
| **Free heap** | 33.86 MB | - | post-WiFi | - |

### TCP Optimization History

| Config | TCP Mbps | Notes |
|--------|----------|-------|
| TCP_NODELAY ON, 1400B sends | 14.62 | Baseline |
| TCP_NODELAY OFF, 1400B sends | 12.45 | Nagle hurts streaming (-15%) |
| TCP_NODELAY ON, 16KB sends | **16.45** | Larger send() reduces syscall overhead |
| + sndbuf 131072 | 16.45 | Combined with above |

**Key finding**: `TCP_NODELAY` must stay ON for bulk streaming. Removing it causes
Nagle's algorithm to create stop-and-wait behavior. Larger `send()` chunks (16KB
instead of 1400B) reduce per-packet overhead — TCP handles segmentation internally.

### Slave Firmware Experiments (OTA via SDIO)

We attempted to optimize the C6 slave firmware via OTA. Key findings:

| Slave Config | UDP Mbps | TCP Mbps | Status |
|-------------|----------|----------|--------|
| **Factory firmware** | **42.18** | **16.45** | Best overall |
| Custom (TX_BA_WIN=32, RX_BA_WIN=32, +buffers) | 39.99 | ~7.3 | Memory pressure |
| Custom (TX_BA_WIN=16, RX_BA_WIN=16) | 42.12 | ~10.7 | Still worse |
| Custom (TX_BA_WIN=6, RX_BA_WIN=6) | 41.28 | ~2 | TX aggregation killed |
| Custom (original config, TX_BA_WIN=32) | 41.56 | 15-18 | Stalls at ~17s |

**Critical discovery**: The factory-default slave config has `TX_BA_WIN=32` and
`RX_BA_WIN=6` (asymmetric). TX direction is the hot path for streaming.
Custom-built slave firmware is NOT identical to the factory firmware even with
the same sdkconfig — likely due to esp-hosted version or compiler differences.

**Recommendation**: Use factory slave firmware for best TCP stability. For features
requiring custom slave code (promiscuous mode, packet injection), accept some TCP
degradation. UDP performance (~42 Mbps) is unaffected.

## Host-Side Optimizations Applied

### sdkconfig.defaults (ESP32-P4 Host)

```ini
# TCP Window & Buffers (CRITICAL — default 5760 caps at ~15 Mbps)
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65534
CONFIG_LWIP_TCP_WND_DEFAULT=65534
CONFIG_LWIP_TCP_RECVMBOX_SIZE=64
CONFIG_LWIP_UDP_RECVMBOX_SIZE=64
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64

# WiFi A-MPDU (larger BA window = more in-flight frames)
CONFIG_WIFI_RMT_TX_BA_WIN=32
CONFIG_WIFI_RMT_RX_BA_WIN=32
CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=32
CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM=64
CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM=64

# lwIP IRAM (~27 KB for packet processing hot path)
CONFIG_LWIP_IRAM_OPTIMIZATION=y
CONFIG_LWIP_EXTRA_IRAM_OPTIMIZATION=y

# TCP Algorithms
CONFIG_LWIP_TCP_SACK_OUT=y
CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=8
CONFIG_LWIP_TCP_RTO_TIME=300

# Task Scheduling (pin lwIP to core 1, SDIO on core 0)
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y
CONFIG_LWIP_TCPIP_CORE_LOCKING=y
```

### Application-Level Optimizations

```c
#define TX_PACKET_SIZE    1400   /* UDP: fit within MTU */
#define TCP_TX_CHUNK_SIZE 16384  /* TCP: larger sends, less overhead */

/* TCP socket options */
setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));  /* ON */
int sndbuf = 131072;
setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
```

## Slave OTA Pipeline

Successfully implemented OTA flashing of C6 slave firmware through the existing
SDIO link. No physical access to C6 UART required.

```
Boot Sequence with OTA:

  P4 Boot
    |
    v
  wifi_init_sta()          <-- Initializes SDIO transport to C6
    |                          (takes ~18s for async SDIO handshake)
    v
  WiFi Connected           <-- Proves SDIO/RPC channel is up
    |
    v
  try_slave_ota()
    |
    +-- Read slave_fw partition (0x210000, 2MB)
    |   |
    |   +-- magic == 0xFF? --> Skip OTA, continue to tests
    |   |
    |   +-- magic == 0xE9? --> Valid firmware found
    |       |
    |       v
    |   esp_hosted_slave_ota_begin()
    |   esp_hosted_slave_ota_write() x N  (1400B chunks)
    |   esp_hosted_slave_ota_end()
    |   esp_hosted_slave_ota_activate()
    |       |
    |       v
    |   Erase partition (prevent re-OTA)
    |   esp_restart()
    |       |
    |       v
    |   [Reboot -- OTA skipped, run tests]
    |
    v
  test_udp_stream()   (30s)
  test_tcp_stream()   (30s)
  test_packet_monitor() (10s)
```

### OTA Flashing Procedure

```bash
# 1. Build slave firmware
cd esp32-c6-slave
idf.py set-target esp32c6
idf.py build

# 2. Erase partition (CRITICAL: ensures 0xFF end-detection works)
esptool.py --chip esp32p4 -p /dev/ttyACM0 \
  erase_region 0x210000 0x200000

# 3. Write slave binary (--force: cross-chip data partition)
esptool.py --chip esp32p4 -p /dev/ttyACM0 \
  write_flash 0x210000 build/network_adapter.bin --force

# 4. On next boot, P4 will OTA the C6 automatically
```

### Partition Table

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x200000,
slave_fw, data, 0x40,    0x210000, 0x200000,
```

## WiFi Raw Packet System (CustomRpc)

Standard esp-hosted RPC does NOT support promiscuous mode or raw TX injection
(RPC IDs 305-310 are defined but not implemented). We built a CustomRpc
extension:

```
+------------------+     CustomRpc      +------------------+
|  P4 Host         |  (msg_id based)    |  C6 Slave        |
|                  |                    |                  |
|  wifi_raw.c      |<--- SDIO RPC ---->|  wifi_raw_slave.c|
|  wifi_raw.h      |                   |  wifi_raw_slave.h|
|  wifi_raw_msgs.h |  (shared header)  |  wifi_raw_msgs.h |
+------------------+                    +------------------+

Message IDs (wifi_raw_msgs.h):
  0x0100  SET_PROMISCUOUS    Host -> Slave
  0x0101  SET_CHANNEL        Host -> Slave
  0x0102  SET_FILTER         Host -> Slave
  0x0103  80211_TX           Host -> Slave (raw frame injection)
  0x0180  CMD_RESPONSE       Slave -> Host (status reply)
  0x0181  PROMISC_PKT        Slave -> Host (captured packet event)
```

### Capabilities Proven

- **Promiscuous mode**: Capture management frames (beacons, probes, auth, deauth)
- **Packet injection**: Raw 802.11 frame transmission via `esp_wifi_80211_tx()`
- **Channel control**: Switch monitoring channel at runtime
- **Filter control**: Select packet types to capture (mgmt/ctrl/data)

### Requirements

- Custom slave firmware with `wifi_raw_slave.c` handlers
- `CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y` on host
- `CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS=8` on host

## Performance Bottleneck Analysis

```
Physical Limit Breakdown:

  WiFi 2.4 GHz HT20 MCS7:  ~72 Mbps PHY rate
  WiFi protocol overhead:    ~40-50% (MAC headers, ACKs, backoff)
  Effective WiFi:            ~36-43 Mbps
  SDIO 4-bit @ 40 MHz:      ~160 Mbps (not the bottleneck)
  UDP measured:              42 Mbps   <-- Near WiFi limit
  TCP measured:              16 Mbps   <-- Limited by ACK round-trip

  For FPV video (1080p30 H.264):
    Typical bitrate:         5-15 Mbps
    Our UDP capacity:        42 Mbps
    Headroom:                3-8x margin
```

## Lessons Learned

1. **HT40 is worse than HT20** on 2.4 GHz — congestion in the 40 MHz band
   causes more retransmissions than the doubled bandwidth provides
2. **TCP_NODELAY is essential** for streaming — Nagle creates stop-and-wait
3. **Larger TCP send chunks** (16KB) dramatically reduce syscall overhead
4. **C6 slave memory is precious** — 320KB SRAM, aggressive buffer config backfires
5. **TX_BA_WIN matters more than RX_BA_WIN** for upload streaming (asymmetric)
6. **OTA must wait for SDIO init** — transport takes ~18s to initialize async
7. **Erase before OTA write** — old flash data breaks 0xFF end-detection
8. **Custom slave != factory slave** — even identical config produces different behavior
