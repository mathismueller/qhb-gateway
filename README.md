# `qhb-gateway`

Minimal, low-wear Buildroot (`BR2_EXTERNAL`) firmware for the **Telekom Qivicon Home Base 2** (Typ 3101 / HiSilicon SD5650T), turning it into a headless wired radio bridge for Home Assistant & RaspberryMatic.

See [ROADMAP.md](ROADMAP.md) for the full architecture, flash layout, and phased plan.

## Exposed Network Protocols

| Subsystem | Port / Transport | Protocol | Consumer |
|---|---|---|---|
| **Zigbee (`EM3587`)** | TCP `2002` | ASHv2 over TCP (`qhb-zigbeed`) | Home Assistant ZHA |
| **HomeMatic (`Si1002`)** | TCP `3008` | `HB-RF-ETH` (`qhb-rfe-daemon`) | RaspberryMatic / OpenCCU |
| **Bluetooth (`RTL8761AUV`)** | TCP `6053` | ESPHome BT Proxy (`qhb-btproxy`) | Home Assistant ESPHome |
| **DECT ULE (`DCX81`)** | MQTT | MQTT + HA Discovery (`qhb-dectd`) | Home Assistant MQTT |

## Quick Start

```bash
# 1. (Optional) Extract proprietary vendor blobs directly from a stock mtd0_rootfs.bin dump
./scripts/extract-vendor.sh --bin /path/to/mtd0_rootfs.bin

# 2. Fetch Buildroot 2025.02.18 LTS & build qhb_defconfig
./scripts/build.sh

# 3. Output artifacts
ls -lh output/images/
```
