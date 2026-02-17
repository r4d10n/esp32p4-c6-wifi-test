# lwIP & WiFi Performance Optimization Study

ESP32-P4 (host) + ESP32-C6 (slave) over SDIO, using esp-hosted v2.11.7.
Board: Waveshare ESP32-P4-WIFI6-NANO.

## Executive Summary

The default ESP-IDF configuration leaves significant throughput on the table. The three biggest bottlenecks are:

1. **TCP window size** (5760 bytes) — hard-caps TCP at ~15 Mbps at 3ms RTT
2. **A-MPDU BA window** (6) — limits WiFi frame aggregation to 6 frames in-flight
3. **lwIP not in IRAM** — every packet traverses flash-cached code with variable latency

With all optimizations applied, expected improvement is **+40-60% TCP throughput** and **+10-15% UDP throughput**.

---

## Current vs Optimized Configuration

### 1. TCP Window & Buffer Sizes (CRITICAL)

The TCP bandwidth-delay product formula: `Throughput = Window_Size / RTT`

| Parameter | Current | Optimized | Impact |
|-----------|---------|-----------|--------|
| `LWIP_TCP_SND_BUF_DEFAULT` | 5760 (4×MSS) | 65534 | TCP TX ceiling: 15→524 Mbps at 3ms RTT |
| `LWIP_TCP_WND_DEFAULT` | 5760 (4×MSS) | 65534 | TCP RX ceiling: 15→524 Mbps at 3ms RTT |
| `LWIP_TCP_RECVMBOX_SIZE` | 6 | 64 | Eliminates RX stalls on burst arrivals |
| `LWIP_UDP_RECVMBOX_SIZE` | 6 | 64 | Eliminates UDP RX drops under load |
| `LWIP_TCPIP_RECVMBOX_SIZE` | 32 | 64 | Larger input queue for tcpip_thread |

**Why this matters**: At 36 Mbps WiFi throughput with ~3ms RTT (WiFi + SDIO), the minimum window needed is `36e6 × 0.003 / 8 = 13,500 bytes`. The current 5760-byte window cannot sustain even half the available bandwidth over TCP. This is the single largest performance bottleneck.

**Memory cost**: TCP window buffers are allocated from heap dynamically. At 65534 bytes × 2 (send + receive) per connection = ~128 KB per active TCP connection. The P4 has 32 MB PSRAM, so this is negligible.

**Note**: For windows larger than 65535 bytes (window scaling, RFC 1323), enable `CONFIG_LWIP_WND_SCALE=y` and set `CONFIG_LWIP_RCV_SCALE=4` or higher. This requires SPIRAM allocation verification but could allow windows up to 1 MB.

### 2. WiFi A-MPDU Aggregation (HIGH)

A-MPDU (Aggregated MAC Protocol Data Unit) bundles multiple WiFi frames into a single transmission, reducing per-frame overhead (preamble, IFS, ACK).

| Parameter | Current | Optimized | Impact |
|-----------|---------|-----------|--------|
| `WIFI_RMT_TX_BA_WIN` | 6 | 32 | 5.3× more frames in-flight for TX |
| `WIFI_RMT_RX_BA_WIN` | 6 | 32 | 5.3× more frames in-flight for RX |
| `WIFI_RMT_STATIC_RX_BUFFER_NUM` | 10 | 32 | Must be >= RX_BA_WIN per ESP-IDF docs |
| `WIFI_RMT_DYNAMIC_RX_BUFFER_NUM` | 32 | 64 | More RX buffer headroom |
| `WIFI_RMT_DYNAMIC_TX_BUFFER_NUM` | 32 | 64 | More TX buffer headroom |

**Why this matters**: With BA window of 6, the WiFi MAC can only have 6 unacknowledged frames in flight. At 1500 bytes per frame, that's 9000 bytes before waiting for a Block ACK. With BA window of 32, up to 48,000 bytes can be in flight — a 5.3× improvement in burst capacity.

**Requirement**: The AP must also support BA window ≥ 32. Most modern WiFi 5/6 APs do. The slave C6 already has `TX_BA_WIN=32` but `RX_BA_WIN=6` — both should be 32.

**Memory cost**: Each static RX buffer is 1600 bytes. Increasing from 10→32 costs +35 KB on the C6 slave (512 KB SRAM). This may be tight — test and monitor free heap.

### 3. lwIP IRAM Placement (HIGH)

| Parameter | Current | Optimized | IRAM Cost | Impact |
|-----------|---------|-----------|-----------|--------|
| `LWIP_IRAM_OPTIMIZATION` | not set | **y** | ~10 KB | Core RX/TX path in IRAM |
| `LWIP_EXTRA_IRAM_OPTIMIZATION` | not set | **y** | ~17 KB | Extended path (100+ functions) |

**Why this matters**: With IRAM placement, the entire lwIP packet processing hot path runs from tightly-coupled SRAM instead of flash cache. This eliminates cache miss stalls during packet processing. Espressif documents >10% throughput improvement on single-core chips; the multi-core P4 benefits less but still gains 5-10%.

**Functions relocated** (from linker fragment analysis):
- `tcp_input`, `tcp_output`, `tcp_receive`, `tcp_write`, `tcp_enqueue`
- `pbuf_alloc`, `pbuf_free`, `pbuf_realloc`, `pbuf_ref`
- `ip4_input`, `ip4_output`, `ip4_route`
- `udp_input`, `udp_sendto`, `udp_send`
- `etharp_output`, `ethernet_input`
- `netif_input`, `netif_loop_output`
- Plus ~80 more functions with `EXTRA_IRAM_OPTIMIZATION`

**Risk**: 27 KB total IRAM consumption. The P4 has 256 KB IRAM — this is ~10%, acceptable.

### 4. TCP Algorithm Tuning (MEDIUM)

| Parameter | Current | Optimized | Impact |
|-----------|---------|-----------|--------|
| `LWIP_TCP_SACK_OUT` | not set | **y** | Selective ACK — faster recovery from packet loss |
| `LWIP_TCP_OOSEQ_MAX_PBUFS` | 4 | 8 | More out-of-order segments buffered |
| `LWIP_TCP_RTO_TIME` | 1500 ms | 300 ms | Faster retransmit timeout (LAN appropriate) |
| `LWIP_TCP_HIGH_SPEED_RETRANSMISSION` | y | y (keep) | Already enabled |

**SACK (Selective Acknowledgment)**: Without SACK, a single lost segment forces retransmission of all subsequent segments (Go-Back-N). With SACK, only the lost segment is retransmitted. On a 2.4 GHz WiFi link with occasional interference, SACK significantly reduces recovery time.

**RTO tuning**: The default 1500ms RTO is designed for Internet paths. On a local WiFi→SDIO path with ~3ms RTT, an RTO of 300ms is more appropriate. This means lost packets are detected and retransmitted 5× faster.

### 5. Task Scheduling (MEDIUM)

| Parameter | Current | Optimized | Impact |
|-----------|---------|-----------|--------|
| `LWIP_TCPIP_TASK_AFFINITY` | NO_AFFINITY | **CPU1** | Dedicated core for network stack |
| `LWIP_TCPIP_TASK_STACK_SIZE` | 3072 | 4096 | Prevents stack overflow under load |
| `LWIP_TCPIP_TASK_PRIO` | 18 | 18 (keep) | Already high priority |
| `LWIP_TCPIP_CORE_LOCKING` | not set | **y** | Mutex-based locking (better for multi-core) |

**Core affinity**: The P4 is dual-core. Pinning the lwIP task to core 1 keeps it off core 0 where the esp-hosted SDIO driver and WiFi tasks run, reducing contention. This is the same approach Espressif uses in their high-performance examples.

**Core locking**: The default message-passing model (`TCPIP_CORE_LOCKING=0`) requires thread context switches for every API call. With core locking enabled, lwIP API calls from the application can execute directly in the caller's context, reducing latency. The slave C6 already uses this (`CONFIG_LWIP_TCPIP_CORE_LOCKING=y`).

### 6. SDIO Transport Tuning (LOW-MEDIUM)

| Parameter | Current | Optimized | Impact |
|-----------|---------|-----------|--------|
| `ESP_HOSTED_SDIO_TX_Q_SIZE` | 40 | 60 | More TX buffering depth |
| `ESP_HOSTED_SDIO_RX_Q_SIZE` | 40 | 60 | More RX buffering depth |
| `ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ` | 40000 | 40000 (keep) | 50 MHz risky without SI testing |

**Queue depth**: The slave TX queue (20) is the binding constraint, not the host. Increasing host queues from 40→60 provides more headroom for burst absorption. The cost is 60 × sizeof(queue_entry) ≈ minimal.

**Clock**: 50 MHz is theoretically possible but requires signal integrity validation on the PCB traces. The 40→50 MHz upgrade would add +25% raw SDIO bandwidth, but since SDIO is already 2.5× faster than WiFi, the real-world gain is small (~2-5 Mbps).

### 7. Checksum Offload (ALREADY OPTIMAL)

| Parameter | Current | Impact |
|-----------|---------|--------|
| `LWIP_CHECKSUM_CHECK_IP` | disabled | Hardware/firmware handles IP checksums |
| `LWIP_CHECKSUM_CHECK_UDP` | disabled | Hardware/firmware handles UDP checksums |
| `LWIP_CHECKSUM_CHECK_ICMP` | enabled | Keep for diagnostics (ping) |

The WiFi hardware computes checksums. Disabling software verification is correct and saves CPU cycles per packet.

### 8. Zero-Copy Path (ALREADY OPTIMAL)

| Parameter | Current | Impact |
|-----------|---------|--------|
| `LWIP_L2_TO_L3_COPY` | disabled | Zero-copy RX path active |
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | disabled | Buffers in DMA-capable internal SRAM |
| `LWIP_NETIF_TX_SINGLE_PBUF` | hardcoded=1 | TX coalesces to single DMA-friendly pbuf |
| `LWIP_SUPPORT_CUSTOM_PBUF` | hardcoded=1 | Zero-copy WiFi RX enabled |

These are all correct. Do NOT enable `L2_TO_L3_COPY` — it forces a memcpy of every received packet, doubling memory bandwidth.

### 9. Memory Architecture (ALREADY OPTIMAL)

| Parameter | Current | Impact |
|-----------|---------|--------|
| `SPIRAM` | enabled (32 MB, 200 MHz hex mode) | Large heap for application data |
| `SPIRAM_MALLOC_ALWAYSINTERNAL` | 16384 | Allocations ≤16 KB use internal SRAM |
| `SPIRAM_MALLOC_RESERVE_INTERNAL` | 32768 | 32 KB internal SRAM always reserved |
| `MEM_LIBC_MALLOC` | hardcoded=1 | Heap-based allocation (not fixed pools) |
| `MEMP_MEM_MALLOC` | hardcoded=1 | Pool allocator bypassed, uses heap |

ESP-IDF uses the C library malloc throughout lwIP, not fixed-size pools. There is no `PBUF_POOL_SIZE` to tune. Buffer allocation comes from the FreeRTOS heap, which is backed by internal SRAM for small allocations and PSRAM for large ones.

---

## Complete Optimized Configuration

### Host P4 — `sdkconfig.defaults` additions

```ini
# ============================================================
# PERFORMANCE OPTIMIZATION - lwIP & WiFi
# ============================================================

# --- TCP Window & Buffers (CRITICAL) ---
# Default 5760 caps TCP at ~15 Mbps. 65534 removes the ceiling.
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65534
CONFIG_LWIP_TCP_WND_DEFAULT=65534
CONFIG_LWIP_TCP_RECVMBOX_SIZE=64
CONFIG_LWIP_UDP_RECVMBOX_SIZE=64
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64

# --- WiFi A-MPDU (HIGH) ---
# BA window 6→32: 5.3× more frames in-flight
CONFIG_WIFI_RMT_TX_BA_WIN=32
CONFIG_WIFI_RMT_RX_BA_WIN=32
CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=32
CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM=64
CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM=64

# --- lwIP IRAM (HIGH) ---
# Move entire packet processing hot path to IRAM (~27 KB)
CONFIG_LWIP_IRAM_OPTIMIZATION=y
CONFIG_LWIP_EXTRA_IRAM_OPTIMIZATION=y

# --- TCP Algorithms (MEDIUM) ---
CONFIG_LWIP_TCP_SACK_OUT=y
CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=8
CONFIG_LWIP_TCP_RTO_TIME=300

# --- Task Scheduling (MEDIUM) ---
# Pin lwIP to core 1 (core 0 handles SDIO/WiFi driver)
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096
CONFIG_LWIP_TCPIP_CORE_LOCKING=y

# --- SDIO Queue Depth (LOW) ---
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=60
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=60
```

### Slave C6 — `sdkconfig.defaults.esp32c6` changes

The slave already has good defaults but needs RX BA window fixed:

```ini
# Fix asymmetric BA window (TX=32 already, RX was 6)
CONFIG_ESP_WIFI_RX_BA_WIN=32
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=32

# Match host TCP settings for split-network scenarios
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65534
CONFIG_LWIP_TCP_WND_DEFAULT=65534
CONFIG_LWIP_TCP_RECVMBOX_SIZE=64
CONFIG_LWIP_UDP_RECVMBOX_SIZE=64
```

---

## Expected Impact Matrix

| Optimization | TCP Gain | UDP Gain | Memory Cost | Risk |
|-------------|----------|----------|-------------|------|
| TCP window 5760→65534 | **+15-20 Mbps** | none | ~128 KB/conn (heap) | None |
| BA window 6→32 | +5-10 Mbps | +3-5 Mbps | +35 KB C6 SRAM | AP must support |
| lwIP IRAM | +2-4 Mbps | +2-4 Mbps | 27 KB IRAM | None |
| TCP SACK | +2-5 Mbps (lossy) | n/a | ~1 KB | None |
| Core affinity | +1-3 Mbps | +1-2 Mbps | None | None |
| Core locking | +1-2 Mbps | negligible | None | Low |
| SDIO queues 40→60 | +1-2 Mbps | +1-2 Mbps | ~3 KB | None |
| RTO 1500→300ms | +1-3 Mbps (lossy) | n/a | None | None |
| **Combined estimate** | **+25-40 Mbps** | **+7-13 Mbps** | | |

### Projected Throughput

| Metric | Before | After (projected) |
|--------|--------|--------------------|
| UDP TX | 40.14 Mbps | 45-50 Mbps |
| UDP RX | ~36 Mbps | 40-48 Mbps |
| TCP TX | ~15 Mbps (window-limited) | 30-40 Mbps |
| TCP RX | ~12 Mbps (window-limited) | 25-35 Mbps |

**Note**: UDP is primarily CPU-bottlenecked on the C6 (single-core 160 MHz RISC-V). The optimization gains for UDP are limited by this architectural ceiling. TCP gains are much larger because the current window-size bottleneck is artificial and fully removable.

---

## Options NOT Changed (and why)

| Option | Current | Why Not Changed |
|--------|---------|-----------------|
| `LWIP_L2_TO_L3_COPY` | disabled | Enables zero-copy RX — changing would halve throughput |
| `LWIP_CHECKSUM_CHECK_IP/UDP` | disabled | Hardware offload active — enabling wastes CPU |
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | disabled | Would move network buffers to PSRAM (slower, not DMA-safe) |
| `LWIP_TCP_MSS` | 1440 | Standard for WiFi; larger causes fragmentation |
| `LWIP_TCP_TMR_INTERVAL` | 250 ms | Standard; reducing increases timer overhead |
| `LWIP_TCP_MSL` | 60000 ms | TIME_WAIT duration; standard value |
| `LWIP_MAX_SOCKETS` | 10 | Sufficient for this application |
| `LWIP_IP_FORWARD` | disabled | Not a router |
| `LWIP_STATS` | disabled | Debug only, adds overhead |
| `ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ` | 40000 | 50 MHz needs signal integrity testing on PCB |
| `FREERTOS_HZ` | 1000 | Already at max useful tick rate |
| `PM_ENABLE` | disabled | Power management correctly disabled for throughput |

---

## Verification Plan

After applying optimizations, run these tests:

### 1. Build Verification
```bash
# Delete sdkconfig to regenerate from defaults
rm sdkconfig
idf.py set-target esp32p4
idf.py build
```

Check for:
- No IRAM overflow errors
- No Kconfig warnings about invalid combinations

### 2. Free Heap Check
Add to application startup:
```c
ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
ESP_LOGI(TAG, "Free internal: %lu bytes", esp_get_free_internal_heap_size());
ESP_LOGI(TAG, "Min free heap: %lu bytes", esp_get_minimum_free_heap_size());
```

Thresholds:
- Internal heap should have >50 KB free after WiFi connect
- Total heap (with PSRAM) should have >1 MB free

### 3. TCP Throughput (iperf)
```bash
# On host PC (server)
iperf3 -s

# On ESP32 (client) - add iperf component or use custom TCP test
# Expected: 30-40 Mbps (vs ~15 Mbps before)
```

### 4. UDP Throughput (existing test)
```bash
# Use existing UDP test in app_main.c
# Expected: 40-50 Mbps sender, 38-45 Mbps receiver
```

### 5. Stability Test
Run continuous throughput for 5 minutes. Monitor for:
- Heap fragmentation (minimum free heap declining)
- SDIO errors in logs
- WiFi disconnections
- C6 slave watchdog resets (if heap exhausted)

---

## Advanced Optimizations (Future)

These require code changes, not just config:

### Window Scaling (RFC 1323)
```ini
CONFIG_LWIP_WND_SCALE=y
CONFIG_LWIP_RCV_SCALE=4
```
Allows TCP windows up to 1 MB (65535 × 2^4 = 1,048,560 bytes). Requires SPIRAM allocation for the window buffer and AP/peer support. Useful if RTT increases (e.g., through a VPN or WAN path).

### Nagle Algorithm Control
The lwIP default enables Nagle (coalesces small writes). For latency-sensitive applications (FPV video, telemetry), disable per-socket:
```c
int flag = 1;
setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
```

### Custom SDIO Buffer Size
The esp-hosted transport uses 1536-byte buffers (`ESP_TRANSPORT_SDIO_MAX_BUF_SIZE`). This is a hard-coded constant, not a Kconfig option. Increasing it requires modifying esp-hosted source and rebuilding both host and slave. Diminishing returns since WiFi MTU is 1500.

### Interrupt Coalescing
The SDIO driver processes one interrupt per received buffer. At high packet rates (~5000 pps), this generates significant ISR overhead. Coalescing multiple packets into a single interrupt could reduce CPU load by 10-20%, but requires SDIO driver modification.

### C6 CPU Frequency
The slave C6 runs at 160 MHz (max for this chip). No further CPU optimization is possible without a different co-processor (e.g., ESP32-C5 at 240 MHz dual-core).

---

## References

- [ESP-IDF lwIP Kconfig](https://github.com/espressif/esp-idf/blob/master/components/lwip/Kconfig) — 1527 lines, all options
- [esp-hosted Performance Optimization](https://github.com/espressif/esp-hosted/blob/master/docs/performance_optimization.md)
- [ESP32-C6 Datasheet](https://www.espressif.com/en/products/socs/esp32-c6) — WiFi 6 PHY specifications
- [lwIP TCP tuning (upstream)](https://www.nongnu.org/lwip/2_1_x/optimization.html)
- [RFC 1323 - TCP Window Scaling](https://datatracker.ietf.org/doc/html/rfc1323)
- [RFC 2018 - TCP SACK](https://datatracker.ietf.org/doc/html/rfc2018)
