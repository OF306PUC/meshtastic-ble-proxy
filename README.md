# Meshtastic BLE Proxy — nRF52840 / Zephyr

A Zephyr firmware for the **Nordic nRF52840** that acts as a **Meshtastic-compatible
BLE peripheral** and **multiplexes up to 6 phones onto a single Meshtastic node** over
UART. Where stock Meshtastic allows only one phone ↔ one node BLE link at a time, this
proxy gives each phone a connection that behaves like a standalone 1:1 link.

## What it does

- **BLE peripheral** exposing the Meshtastic GATT service; up to **6 simultaneous**
  phone connections.
- **Per-phone config-session virtualization:** the proxy issues its own `want_config`
  to the node at boot, caches the FromRadio burst, and **replays it to each phone**
  with that phone's nonce echoed in `config_complete_id` (serve-on-read, O(1) RAM).
  Handles the app's special `want_config` nonces (`69420` config-only / `69421`
  nodes-only) via per-nonce segmentation.
- **ToRadio → node:** each phone's writes are forwarded to the node over UART.
- **FromRadio → phones:** broadcast to all connections, or **targeted** by a custom
  proxy header (`PROXY_PORTNUM 256` + `DST_ID`) for per-phone addressing.
- **Liveness (v1):** upstream UART keepalive (`k_work_delayable`, ~5 min idle) +
  reactive synthesized `queueStatus` reply to each phone's heartbeat.

## Hardware

- **Board:** nRF52840 DK (`boards/nrf52840dk_nrf52840.overlay`).
- **UART1 → Meshtastic node:** `P1.01` RX / `P1.02` TX, 115200 8N1 (Stream API framing
  `0x94 0xC3 len_hi len_lo …`).
- **UART0:** JLink RTT logging (independent of the mesh link).

## Build (nRF Connect SDK / Zephyr)

Built and tested against **NCS v2.7.0** (Zephyr + nanopb).

### Protobuf dependency (REQUIRED before building)

The build generates nanopb C sources from the **Meshtastic `.proto` definitions**,
expected at **`./proto/meshtastic/`**. They are **not** included in this repo — provide
them first:

```sh
# Option A — symlink a local Meshtastic protobufs checkout:
ln -s /path/to/meshtastic/protobufs proto

# Option B — git submodule:
git submodule add https://github.com/meshtastic/protobufs proto
```

`CMakeLists.txt` reads these files from `proto/meshtastic/`:
`mesh`, `portnums`, `channel`, `config`, `device_ui`, `module_config`, `atak`,
`telemetry`, `xmodem` (`.proto`).

### Configure & flash

```sh
west build -b nrf52840dk_nrf52840
west flash
```

Set the BLE device name in `prj.conf` before flashing — it must match
`^.*_([0-9a-fA-F]{4})$` (a 4-hex suffix), e.g. `Meshtastic_CA1E`.

## Host unit tests (no hardware)

`tests/` holds host-side gcc tests (each file's top comment has its exact compile line;
they shim the few Zephyr/nanopb headers they need):

- `proto_handler_test.c` — `ToRadio` decode + `config_complete`/`heartbeat`/`queueStatus`
  encoders (round-trip).
- `config_cache_test.c` — arena packing, overflow-without-corruption, atomic ready barrier.
- `config_cache_segment_test.c` — `want_config` special-nonce segmentation (data-driven
  against real captured bursts).

## Source layout (`src/`)

| File | Responsibility |
|---|---|
| `main.c` | Boot order; routes ToRadio (want_config/heartbeat handled locally, packets → UART) |
| `ble_gatt.c/.h` | GATT service, per-phone state, serve-on-read replay, queueStatus reply |
| `uart_meshtastic.c/.h` | UART1 async/DMA driver, Stream API framing, TX queue |
| `proto_handler.c/.h` | nanopb decode (FromRadio/ToRadio) + encoders (config_complete/heartbeat/queueStatus) |
| `proxy_protocol.c/.h` | Custom proxy header (VERSION/SRC/DST/content), `PROXY_PORTNUM 256` |
| `router.c/.h` | FromRadio dispatch: fetch→cache, LIVE→broadcast / targeted; keepalive swallow |
| `config_cache.c/.h` | Packed cache of the boot config burst; per-nonce segmentation |
| `upstream_session.c/.h` | Boot `want_config`, BOOT→FETCHING→CACHE_READY→LIVE, UART keepalive |

## Phone-app integration (summary)

- **Stock Meshtastic app:** works as-is via **broadcast** — every phone behaves like a
  client of the same node (shared `nodenum`, shared config).
- **Per-phone addressing (router):** a custom/modified app registers its `proxy_id`
  (NODE_REG), frames messages with the proxy header inside a `portnum 256` MeshPacket,
  and maintains a `proxy_id → nodenum` directory. Unregistered `DST_ID`s fall back to
  broadcast.

**→ Full integration guide for app teams: [`docs/CLIENT-INTEGRATION.md`](docs/CLIENT-INTEGRATION.md)**
(GATT UUIDs, connection lifecycle, wire format, the four app changes, gotchas).

## Status

**v1.0** — per-phone config virtualization + segmentation + heartbeats; validated on
hardware with 3 concurrent phones (full config, mesh messaging + ACKs).
**Deferred (v1.1):** hardware watchdog (WDT) and an upstream liveness watchdog
(detect a dead node).

## License

TBD. Note the Meshtastic protobuf definitions pulled in at build time are separately
licensed by the Meshtastic project.
