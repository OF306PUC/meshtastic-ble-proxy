# `tools/meshtastic/` — node provisioning & inspection scripts

Host-side Python utilities for provisioning the LILYGO T-Beam Meshtastic nodes
that hang off the nRF52840 BLE proxy (channels, LoRa region, serial module,
telemetry/GPS intervals) and for inspecting a node's health. These talk to a
node over **direct USB serial** — they are independent of the nRF52840 BLE
proxy firmware.

> **Flooding is intentional.** The telemetry and position broadcast intervals in
> `configure_params.py` are deliberately low (30 s / 60 s) to stress/flood the
> mesh for testing. A production deployment would raise them (~900 s / ~1800 s).

## Setup

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Essentials are [`meshtastic`](https://pypi.org/project/meshtastic/) (CLI +
Python API) and `pyserial`; `requirements.txt` pins the full environment.

### Secrets — `.env`

Channel PSKs (base64) are read from `.env` **in this directory**. It is
**git-ignored** — never commit it. Set both keys, identical on every node in the
mesh:

```sh
LORA_TELEMETRY_CHANNEL_PSK=base64:...
LORA_MSG_CHANNEL_PSK=base64:...
```

## Scripts

### `radio_config.py` — mesh-wide radio settings (single source of truth)

Channel indices/names, LoRa region & preset, rebroadcast mode, and the PSKs
(loaded from `.env`). Imported by the others so settings can't drift. Channel 0
= telemetry (`telCPS_RTC`), channel 1 = messaging (`msgPUC_NET`).

### `configure_params.py` — proxy-node parameters (edit before configuring)

Constants for the proxy-attached node: Bluetooth off (the nRF52840 serves BLE),
telemetry/GPS intervals, and the **serial module** (`PROTO` mode on ESP32
`UART_DEV(1)`, GPIO15 TX / GPIO35 RX — the UART link the proxy attaches to).
Re-exports the shared radio settings from `radio_config.py`.

### `configure.py` — apply the config to a node, then reboot

Drives the `meshtastic` CLI (and the Python API for PSKs) to write the full
config, verifies `rebroadcast_mode` persisted, and reboots. `DEVICE_ROLE` and
`HOP_LIMIT` are set in `configure_params.py` (edit before flashing each board).

```sh
python3 tools/meshtastic/configure.py --port /dev/ttyUSB0
```

Order: LoRa (region/preset/hop_limit) → device (rebroadcast + verify, then role)
→ Bluetooth → channels (rename ch0, add ch1) → **PSKs** → telemetry → serial
module → GPS → reboot. Transient serial errors are retried (`MAX_RETRIES`).

### `check_node_info.py` — inspect & health-check a node

Reports local identity/telemetry, reads back every setting the configure script
touches, and health-checks region/preset/rebroadcast/channels/PSKs against
`radio_config.py`. Exit code 1 on any drift/mismatch, so it doubles as a smoke
check. PSKs are compared but never printed.

```sh
python3 tools/meshtastic/check_node_info.py                 # auto-detect port
python3 tools/meshtastic/check_node_info.py --port /dev/ttyACM0
```

## Ports

T-Beam (ESP32) typically enumerates as `/dev/ttyUSB*`; the nRF52840-DK as
`/dev/ttyACM*`. Pass `--port` explicitly when several devices are attached.
