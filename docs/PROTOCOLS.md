# Networked Radio Protocols

`qhb-gateway` runs **zero smart-home application logic** on the Home Base 2. All radios are exposed as thin, standard wire-protocol bridges so Home Assistant / RaspberryMatic can be upgraded independently for years.

## 1. Zigbee (`EM3587`) — ASHv2 over TCP (`2002`)
- **Daemon:** `qhb-zigbeed` (`/dev/ttyspi0` → TCP `2002`)
- **Home Assistant (ZHA):** Add Zigbee Home Automation → Manual → **EZSP** → `socket://<qhb-ip>:2002` (baudrate `115200`, flow control: none).

## 2. HomeMatic / HmIP (`Si1002`) — `HB-RF-ETH` (`3008`)
- **Daemon:** `qhb-rfe-daemon` (`/dev/ttyAMA1` → port `3008`)
- **RaspberryMatic / OpenCCU:** Configure `hb_rf_eth` with the `qhb` IP address.

## 3. Bluetooth (`RTL8761AUV`) — ESPHome Bluetooth Proxy (`6053`)
- **Daemon:** `qhb-btproxy` (`hci0` → TCP `6053`)
- **Home Assistant:** Discovered automatically (or add **ESPHome** integration → `<qhb-ip>:6053`).

## 4. DECT ULE (`DCX81`) — MQTT + HA Discovery
- **Daemon:** `voiper` + `qhb-dectd`
- **Configuration:** Set `QHB_MQTT_HOST`, `QHB_MQTT_PORT`, `QHB_MQTT_USER`, and `QHB_MQTT_PASS` in `/data/qhb.conf`.

## Configuration (`/data/qhb.conf`)
All settings live in a single declarative file on the persistent UBIFS `/data` partition. See `/etc/qhb/qhb.conf.default` for all options.
