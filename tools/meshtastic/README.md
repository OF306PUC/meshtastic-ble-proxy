# `tools/meshtastic/` — node provisioning & capture scripts

Host-side Python utilities for the proof-of-concept: provisioning the LILYGO
T-Beam Meshtastic nodes (channels, LoRa region, serial module, etc.) and
capturing a node's `want_config` burst over USB serial. These talk to a node
over **direct USB serial** — they are independent of the nRF52840 BLE proxy
firmware.

## Setup

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

`requirements.txt` pins the full environment; the essentials are
[`meshtastic`](https://pypi.org/project/meshtastic/) (CLI + Python API) and
`pyserial`. `fetch_node_config.py` only strictly needs `pyserial` (it speaks the
Stream API directly); `meshtastic` is an optional enhancement there.

## Scripts

### `param_node.py` — central node parameters (edit this first)

Plain constants module imported by `configure_node.py`. Set per-node values here
before configuring: channel base name / count, LoRa region & preset,
Bluetooth, telemetry, GPS, role, and the **serial module** settings.

> **Relevant to the proxy wiring:** `SERIAL_MODULE_MODE = "PROTO"` with
> `SERIAL_MODULE_TXD = 15` / `SERIAL_MODULE_RXD = 35` routes the node's
> Stream-API protobuf traffic onto ESP32 `UART_DEV(1)` (GPIO15 TX / GPIO35 RX) —
> this is the UART link the nRF52840 proxy attaches to (see `docs/HARDWARE.md`).
> `HOP_LIMIT` is derived per node from `REQUIRED_HOPS_TO_GATEWAY`.

### `configure_node.py` — apply the parameters to a node

Drives the `meshtastic` CLI (and the Python API for PSKs) to write the full
config from `param_node.py`, then reboots the node. Runnable from any directory
(it adds its own location to `sys.path` so `import param_node` resolves):

```sh
python3 tools/meshtastic/configure_node.py --port /dev/ttyUSB0
```

What it does, in order: LoRa (region/preset/hop_limit) → device
(rebroadcast/role) → Bluetooth → channels (rename ch0, add ch1+) → **PSKs** →
telemetry → serial module → GPS → reboot. Transient serial errors are retried
(`MAX_RETRIES`).

> **🔑 Secrets — `channel_psks.txt`.** On first run the script generates one
> 32-byte PSK per channel and writes them to `channel_psks.txt` **in this
> directory**; later runs reload that file so every node in the mesh gets
> identical keys. This file is **secret** and is **git-ignored** — never commit
> it, and copy it between machines over a secure channel only. Delete it to
> rotate keys (all nodes must then be re-provisioned).

### `fetch_node_config.py` — capture the `want_config` burst

Triggers the firmware's `want_config` handshake over USB serial and records the
exact ordered `FromRadio` frames the node emits, terminated by the
`config_complete_id` echoing the nonce we sent. Used to (a) verify the FETCHING
ground truth and (b) size the firmware's `CONFIG_CACHE_ARENA_BYTES` from the
real summed payload bytes.

```sh
# Auto-detect port, random nonce:
python3 fetch_node_config.py

# Pin port + nonce and dump raw frames for replay into the host unit test:
python3 fetch_node_config.py --port /dev/ttyACM0 --nonce 305419896 \
    --dump .tmp/config_burst.hex

# Longer timeout for a slow / busy node:
python3 fetch_node_config.py --timeout 30
```

Exit codes: `0` success (matching `config_complete_id` seen), `1`
timeout / no port / serial error, `2` bad arguments.

## Ports

T-Beam (ESP32) typically enumerates as `/dev/ttyUSB*`; the nRF52840-DK as
`/dev/ttyACM*`. Pass `--port` explicitly when several devices are attached.
