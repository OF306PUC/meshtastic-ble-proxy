# Architecture — Meshtastic BLE Proxy (nRF52840 / Zephyr)

This document audits the embedded software: module structure, data flow, the two
state machines, the `want_config` handshake sequence, the threading model, and the
boot order. Diagrams are Mermaid (render natively on GitHub and in the IDE).

The firmware turns one Meshtastic node into a **6-phone BLE front end**. Each phone
gets a connection that behaves like a standalone 1:1 Meshtastic link, while the proxy
arbitrates the single shared UART link to the node.

---

## 1. Module dependency graph

Who calls/includes whom (`src/`). `upstream_session` deliberately has **no** compile
dependency on `ble_gatt` — the per-phone replay is reached through a registered
callback (dependency inversion).

```mermaid
flowchart TD
    main["main.c<br/>boot order · ToRadio/FromRadio callbacks"]
    ble["ble_gatt.c<br/>GATT server · per-phone state · replay"]
    uart["uart_meshtastic.c<br/>UART1 Stream API · TX queue"]
    router["router.c<br/>FromRadio dispatch"]
    up["upstream_session.c<br/>boot want_config · state machine · keepalive"]
    cache["config_cache.c<br/>packed burst cache · segmentation"]
    proto["proto_handler.c<br/>nanopb decode/encode"]
    proxyp["proxy_protocol.c<br/>DST_ID header (portnum 256)"]

    main --> ble
    main --> uart
    main --> proto
    main --> router
    main --> up

    router --> up
    router --> ble
    router --> proxyp
    router --> proto

    ble --> cache
    ble --> proto
    ble --> up

    up --> cache
    up --> uart
    up --> proto

    up -. "serve callback<br/>(set at boot, no static dep)" .-> ble

    classDef new fill:#cfe8ff,stroke:#2b6cb0,color:#14213d;
    class cache,up new;
```

- Pure / leaf modules (no intra-project deps): `proto_handler`, `proxy_protocol`,
  `uart_meshtastic`, `config_cache`.
- The dashed edge is the runtime callback `upstream_set_serve_cb(ble_gatt_replay_cached_burst)`
  registered by `main` at boot — keeps the layering clean (upstream → ble_gatt only at runtime).

---

## 2. Data flow (BLE ↔ UART)

```mermaid
flowchart LR
    subgraph Phones["Up to 6 phones (BLE centrals)"]
        pa["Phone A"]
        pb["Phone B"]
        pn["Phone N"]
    end

    subgraph Proxy["nRF52840 proxy"]
        gatt["ble_gatt<br/>FROMNUM / FROMRADIO / TORADIO / NODE_REG"]
        rt["router"]
        cc["config_cache"]
        us["upstream_session"]
        ut["uart_meshtastic (UART1)"]
    end

    node["Meshtastic node"]
    mesh(("LoRa mesh"))

    pa & pb & pn -- "ToRadio (write)" --> gatt
    gatt -- "want_config / heartbeat<br/>(handled locally)" --> us
    gatt -- "mesh packet" --> ut
    ut -- "Stream API 0x94 0xC3" --> node
    node --> mesh

    node -- "FromRadio" --> ut
    ut --> rt
    rt -- "FETCHING: cache the burst" --> cc
    cc -. "replay (serve-on-read)" .-> gatt
    rt -- "LIVE: broadcast / DST_ID targeted" --> gatt
    gatt -- "FROMRADIO read + FROMNUM notify" --> pa & pb & pn

    classDef store fill:#fde9c8,stroke:#b7791f,color:#3b2f00;
    class cc store;
```

**Two delivery modes in LIVE** (`router.c`):
- **Broadcast** — standard Meshtastic portnums → all connections (stock app works).
- **Targeted** — `portnum == PROXY_PORTNUM (256)` → parse the proxy header, deliver to
  the connection whose registered `proxy_id` matches `DST_ID`; broadcast fallback if
  unregistered (no silent drop).

---

## 3. State machines

### 3a. State machines — upstream session + per-phone connection (unified)

The two state machines that drive onboarding, shown together: the **global upstream
session** (`upstream_session.c`, one instance — `BOOT → FETCHING → CACHE_READY → LIVE`)
and the **per-phone connection** machine (`ble_gatt.c`, one per BLE connection, up to 6 —
`CONNECTED → AWAIT_WANT_CONFIG → {PENDING} → REPLAYING → ACTIVE`).

<p align="center">
  <img src="figs/state-machines.svg" alt="Upstream-session and per-phone connection state machines (unified)" width="65%">
</p>

<p align="center"><em>Figure 3a — left: the single upstream session that fetches the node config once into the shared cache. Right: the per-phone machine; a phone that connects before the cache is ready waits in <code>PENDING</code> and is served automatically once the upstream reaches LIVE.</em></p>

**Two `want_config` rounds.** The phone may run **two `want_config` rounds** with special
nonces — `69420` (ONLY_CONFIG) then `69421` (ONLY_NODES) — each re-arming `REPLAYING`
with a nonce-specific cache segment.

**The `config_complete_id` terminator.** In the Meshtastic protocol,
`FromRadio{config_complete_id = N}` is the **terminator** of a `want_config` burst. The
client treats it as "the config download keyed by nonce `N` is now complete." A phone
that sent `want_config_id = P` is specifically waiting for a `config_complete_id` equal to
`P` — that is its signal to leave the config-download state and go `ACTIVE`. The proxy
therefore never replays the cached terminator (it carries the proxy's boot nonce `R`); it
**synthesizes** a fresh one carrying each phone's own `P` (see §4).

### 3b. System lifecycle — transient vs steady state

The whole system goes through a **transient** phase (boot configuration + connection
setup) before settling into a **steady** phase where text messaging flows naturally.
§3a is the precise per-machine view; this is the system-level overview.

- **TRANSIENT** = the proxy fetches the node's config once, then each phone connects and
  runs its `want_config` handshake (config round `69420` → node-DB round `69421`). These
  are state-changing, one-time-per-(boot / connection) flows — the detailed mechanics live
  in the §3a machines.
- **STEADY** = `OPERATIONAL`: no state changes — the self-loops are the recurring,
  event-driven message flows (UART RX callbacks delivering text/telemetry → routed BLE
  notifications; phone writes → UART → mesh; liveness). This is the "stationary" regime.

<p align="center">
  <img src="figs/steady-state.svg" alt="Steady-state operational message flows" width="65%">
</p>

<p align="center"><em>Figure 3b — the steady (<code>OPERATIONAL</code>) regime: recurring, event-driven flows once the cache is LIVE and the phone is ACTIVE. (TODO: this figure still omits the broadcast path — node FromRadio fanned out to all connected phones.)</em></p>

Notes:
- A phone connecting **before** the cache is ready waits as `PENDING` (§3a) inside the
  transient phase, then onboards automatically once the upstream reaches LIVE.
- Concurrency: the upstream session is global (one); the per-phone machine is
  per-connection (up to 6); they overlap in time. The happy-path ordering holds because a
  real phone simply can't finish onboarding until the cache exists.
- Leaving STEADY → TRANSIENT is per-phone and local: one phone re-running `want_config`
  (or reconnecting) re-enters its onboarding without disturbing the others or the node.

---

## 4. `want_config` handshake + boot fetch (sequence)

```mermaid
sequenceDiagram
    autonumber
    participant N as Meshtastic node
    participant U as uart_meshtastic
    participant US as upstream_session
    participant CC as config_cache
    participant BG as ble_gatt
    participant P as Phone

    Note over US,N: BOOT — proxy fetches ONCE with its own random nonce R
    US->>N: ToRadio{want_config_id = R} (R = random, ≠ 0/1)
    N-->>U: FromRadio burst (my_info, node_info, config, …)
    U->>US: on_fromradio (per frame)
    US->>CC: cache_add_frame(...)  [FETCHING]
    N-->>U: FromRadio{config_complete_id = R}
    US->>CC: cache_mark_ready()  → LIVE
    Note over CC: R-terminator is cached but NEVER replayed<br/>(would leak the boot nonce to phones)

    Note over P,BG: Phone connects (after CACHE_READY) — sends ITS OWN nonce P
    P->>BG: subscribe FROMNUM
    P->>BG: ToRadio{want_config_id = P}

    Note over BG,CC: P's VALUE selects the replay segment
    alt P == 69420 (ONLY_CONFIG) or P == 69421 (ONLY_NODES)
        Note over BG,CC: replay a SUBSET (config+own node_info / node_info-only)
    else any other P  (typical — full config request)
        Note over BG,CC: replay the FULL burst
    end

    BG->>BG: arm replay (cursor=0), encode fresh cc-frame carrying P
    BG-->>P: FROMNUM notify (kick)
    loop drain until empty
        P->>BG: read FROMRADIO
        BG->>CC: cache_frame_in_segment(idx, P)?
        BG-->>P: next in-segment cached frame
    end
    BG-->>P: synthesized FromRadio{config_complete_id = P}
    Note over P: echoed P matches the nonce the phone sent →<br/>want_config COMPLETES → ACTIVE.<br/>(wrong/absent echo ⇒ phone never completes → timeout + retry)

    Note over P,N: Steady state (LIVE)
    P->>BG: ToRadio{heartbeat}
    BG-->>P: synthesized queueStatus (liveness)
    P->>BG: ToRadio{mesh packet}
    BG->>U: forward → node → mesh
```

**Nonce roles (the key idea):**
- **R** — the proxy's own boot nonce. Used *once*, proxy→node, to build the shared cache. Never seen by a phone.
- **P** — each phone's own nonce. Never reaches the node. It has exactly two jobs: (1) its **value** selects the replay segment — `69420`/`69421` request a subset, *any other value* gets the full burst; (2) it is **echoed back** verbatim in the synthesized `config_complete_id` so the phone recognizes the burst as the answer to *its* request and completes its `want_config` state machine.

`config_complete_id` is **never cached** — it is the terminator; the replay synthesizes
a fresh one carrying each phone's own nonce `P`.

> The real Meshtastic Android client typically runs **two** rounds — `want_config(69420)`
> then `want_config(69421)` — to fetch config and the node DB separately (see §3a/§3b).
> Both are just specific values of `P`; a client issuing a single arbitrary nonce would
> instead receive the full burst in one round.

### 4b. FROMRADIO read → per-connection drain

How a phone's ATT reads resolve to its own queue. The `conn` is provided by the BLE
stack (it knows which link the read arrived on); `find_slot(conn)` maps it to that
phone's `proxy_conn` by **pointer identity** — `bt_conn_ref()` is just refcounting, it
assigns no id.

```mermaid
sequenceDiagram
    autonumber
    participant P as Phone (central)
    participant S as Zephyr BLE stack
    participant FR as fromradio_read()
    participant PC as proxy_conn (conns[i])

    Note over S,PC: on connect: on_connected(conn) → alloc_slot → bt_conn_ref(conn),<br/>store pointer in a free conns[] slot (= this phone's state)
    loop drain until a 0-length read
        P->>S: ATT Read Request (FROMRADIO handle)
        S->>FR: fromradio_read(conn, …)  [conn = this link]
        FR->>FR: find_slot(conn) → pc  (conns[i].conn == conn)
        alt REPLAYING
            FR->>PC: next in-segment cache frame / synthesized config_complete
        else ACTIVE
            FR->>PC: dequeue queue[head] (head++, count--)
        end
        FR-->>S: bytes, or 0 when drained
        S-->>P: ATT Read Response
    end
```

Different phones hold different `conn` pointers → different `pc` → **independent queues
drained in parallel**. That pointer-keyed mapping is the whole basis of the multiplexing.

---

## 5. Threading model

Two execution contexts; `config_cache` is published across them by a Zephyr `atomic_t`
release/acquire barrier (`cache_mark_ready` / `cache_is_ready`), so no mutex is needed
for the read-only cache. The per-connection FROMRADIO queue is guarded by a per-`conn`
`k_mutex`.

```mermaid
flowchart TB
    subgraph SWQ["System work queue (single thread)"]
        rxw["rx_work: UART RX → frame assembly"]
        onfr["on_fromradio_uart → proto_decode → router_dispatch"]
        txw["tx_work: drain TX queue → uart_tx"]
        kaw["keepalive_work (k_work_delayable, ~5 min)"]
        rxw --> onfr
    end

    subgraph BTRX["Bluetooth RX thread"]
        tw["toradio_write → on_toradio_ble<br/>(want_config / heartbeat / packet)"]
        fr["fromradio_read (serve-on-read replay / queue)"]
        ccc["CCC + connected/disconnected"]
    end

    cache[("config_cache<br/>read-only after ready")]
    onfr -- writes (FETCHING) --> cache
    fr -- reads (replay) --> cache
    onfr -. "atomic publish" .-> fr

    tw -- "mesh packet" --> txw
    kaw -- "ToRadio.heartbeat" --> txw

    classDef store fill:#fde9c8,stroke:#b7791f,color:#3b2f00;
    class cache store;
```

**Invariant:** all cache **writes** happen on the system work queue during `FETCHING`;
**reads** (per-phone replay) happen on the BT RX thread but only after `cache_is_ready()`
returns true. `k_work_delayable` (not `k_timer`) is used for the keepalive precisely so
it runs in work-queue context and may touch the UART safely.

---

## 6. Boot sequence (`main.c`)

```mermaid
flowchart LR
    a["ble_gatt_init<br/>(register TORADIO cb)"] --> b["bt_enable"]
    b --> c["ble_gatt_start_advertising"]
    c --> d["uart_meshtastic_init<br/>(start DMA RX)"]
    d --> e["upstream_set_serve_cb<br/>(ble_gatt_replay_cached_burst)"]
    e --> f["upstream_session_start<br/>(send want_config, arm keepalive)"]
    f --> g["main loop: k_sleep"]
```

Order matters: GATT is registered before `bt_enable`; the serve callback is registered
before `upstream_session_start` so a PENDING phone can be served the moment the cache is
ready.

---

## 7. Module reference

| File | Responsibility | Context |
|---|---|---|
| `main.c` | Boot order; `on_toradio_ble` (local handling of want_config/heartbeat, forward packets, reschedule keepalive); `on_fromradio_uart` | BT RX + work queue |
| `ble_gatt.c/.h` | GATT service (FROMNUM/FROMRADIO/TORADIO/LOGRADIO/NODE_REG), per-phone state, serve-on-read replay, synthesized queueStatus | BT RX |
| `uart_meshtastic.c/.h` | UART1 async/DMA, Stream API framing (`0x94 0xC3 len_hi len_lo`), RX state machine, TX queue | work queue |
| `proto_handler.c/.h` | nanopb decode (FromRadio/ToRadio) + encoders (config_complete / heartbeat / queueStatus) | both (stack-local encode) |
| `proxy_protocol.c/.h` | Custom proxy header parse/build (VERSION/SRC/DST/content), `PROXY_PORTNUM 256` | pure |
| `router.c/.h` | FromRadio dispatch: FETCHING→cache, LIVE→broadcast / DST_ID targeted; keepalive queueStatus swallow | work queue |
| `config_cache.c/.h` | Packed contiguous arena + index of the boot burst; per-nonce segmentation; atomic ready barrier; queueStatus lookup | written on WQ, read on BT RX |
| `upstream_session.c/.h` | Boot `want_config`, BOOT→FETCHING→CACHE_READY→LIVE, Phase 0 instrumentation, UART keepalive | work queue |

## 8. Key invariants (audit checklist)

- **Serve-on-read replay** — one cached frame per FROMRADIO read; never pre-enqueue the
  burst (would overflow the 8-deep per-conn queue).
- **`cache_mark_ready()` is a release barrier** — readers seeing `cache_is_ready()` see
  the fully-written arena.
- **Burst order preserved**; `config_complete_id` synthesized per phone, never cached.
- **want_config / heartbeat never reach UART**; only mesh packets do.
- **Keepalive** fires only after ~5 min of no real ToRadio (rescheduled on each real TX),
  nonce ≠ 1; its queueStatus reply is swallowed in `router`.
- **No silent drops** — overflow / unregistered DST_ID fall back to broadcast and log.

> Companion: firmware design rationale in the studio's `ADR-001`. Phone-app integration
> (broadcast vs router, NODE_REG, portnum-256 framing) in `client-integration.md`.
