# Reconnect persistence — design proposal

**Status:** proposal — not yet implemented. No source has been modified.
**Scope:** `src/ble_gatt.c` (and its header). Purely additive.

## Problem

Today every scrap of per-phone state is tied to the live `struct bt_conn *`.
When the phone disconnects, `on_disconnected()` calls `free_slot()`, which wipes
the slot completely:

- `src/ble_gatt.c:181` — `free_slot()` does `bt_conn_unref(pc->conn); pc->conn = NULL;`
  and then `memset(&pc->proxy_id, 0, …)`.
- `src/ble_gatt.c:161` — on the next connection `alloc_slot()` re-zeroes
  `fromnum`, the queue, `state = PHONE_CONNECTED`, `nonce`, `replay_cursor`, etc.

Consequence: a phone that drops and comes back is a brand-new stranger. It must
re-register its `proxy_id` via NODE_REG **and** replay the entire config session
(the `want_config` → cached-burst → `config_complete_id` sequence served by
`fromradio_read()`, `src/ble_gatt.c:234`). Nothing is remembered across the gap.

The `bt_conn` pointer itself is *not* an identity we can persist — it is a
reference-counted object owned by the Zephyr stack, valid only while connected,
and a reconnect yields a different pointer. The only durable identity we have is
the phone-supplied `proxy_id` (registered via NODE_REG, `src/ble_gatt.c:559`).

## Goal

Let a returning phone, identified by its `proxy_id`, **skip the full config
replay** and **resume its `fromnum` counter**, instead of restarting from zero.

## Design

Add a second, small table that outlives `free_slot()` — a registry keyed by
`proxy_id` rather than by `bt_conn`. Live slots (`conns[]`) stay exactly as they
are; the registry is a persistence side-car.

```
   conns[]           (live, keyed by bt_conn*) — unchanged
      │  save on disconnect          restore on NODE_REG
      ▼                              ▲
   phone_registry[]  (persistent, keyed by proxy_id)  ← NEW
```

### Data structures (new)

```c
/* Bounds RAM; >= MAX_BLE_CONNECTIONS so no live phone is ever un-recordable. */
#define MAX_KNOWN_PHONES  8U

struct phone_record {
    proxy_id_t proxy_id;        /* key; all-zero => empty slot            */
    uint32_t   fromnum;         /* last FROMNUM counter served            */
    bool       config_done;     /* true once config_complete was reached  */
    uint32_t   last_seen_ms;    /* k_uptime for LRU eviction              */
};

static struct phone_record phone_registry[MAX_KNOWN_PHONES];
```

Deliberately **not** persisted: the outbound FromRadio queue and staging buffer.
They are bounded RAM and their contents are stale the moment the link drops;
re-delivering them on reconnect is wrong, not helpful.

### Helper functions (new, file-static)

```c
/* Find the record for an id, or NULL. Zero id never matches. */
static struct phone_record *registry_find(const proxy_id_t *id);

/* Return an existing record for id, else an empty slot, else LRU-evict.
 * Never returns NULL (eviction guarantees a slot). */
static struct phone_record *registry_alloc(const proxy_id_t *id);

/* Persist a live slot's durable state into the registry.
 * No-op if pc->proxy_id is still zero (phone never registered). */
static void registry_save(const struct proxy_conn *pc);

/* Restore durable state from a record into a freshly allocated live slot. */
static void registry_restore(struct proxy_conn *pc,
                             const struct phone_record *rec);
```

### Integration points (edits to existing functions)

1. **Save on disconnect** — `on_disconnected()` / `free_slot()`,
   `src/ble_gatt.c:429` and `:181`.
   Before `free_slot()` blows the slot away, call `registry_save(pc)` while the
   `proxy_id` and counters are still populated:

   ```c
   static void on_disconnected(struct bt_conn *conn, uint8_t reason)
   {
       struct proxy_conn *pc = find_slot(conn);
       if (pc) {
           registry_save(pc);          /* NEW: capture durable state    */
           free_slot(pc);
           active_conn_count--;
       }
       ble_gatt_start_advertising();
   }
   ```

2. **Restore on re-register** — `ble_gatt_register_proxy_id()`,
   `src/ble_gatt.c:559`.
   After copying the id into `pc->proxy_id`, look it up. On a hit with
   `config_done`, restore `fromnum` and short-circuit the config session:

   ```c
   int ble_gatt_register_proxy_id(struct bt_conn *conn, const proxy_id_t *id)
   {
       struct proxy_conn *pc = find_slot(conn);
       if (!pc) return -ENOENT;

       memcpy(&pc->proxy_id, id, sizeof(proxy_id_t));

       struct phone_record *rec = registry_find(id);   /* NEW */
       if (rec && rec->config_done) {
           registry_restore(pc, rec);                  /* fromnum, state=ACTIVE */
           /* replay stays disarmed: cc_len = 0, replay_cursor untouched */
       }
       return 0;
   }
   ```

## Details, caveats, open questions

- **RAM-only persistence.** The registry lives in `.bss`; it survives a
  disconnect but **not** an nRF reboot/power-cycle. If cross-reboot survival is
  needed, back it with Zephyr `settings`/NVS — a follow-up, out of scope here.

- **Sequencing dependency.** The skip-replay path only helps if NODE_REG arrives
  **before** the client's `want_config` arms the replay burst
  (`fromradio_read()`, `src/ble_gatt.c:234`). If a client always sends
  `want_config` first, this optimization won't trigger; in that case persistence
  still buys `fromnum` continuity but not a shorter handshake. Worth confirming
  against the real Android client order in `docs/client-integration.md`.

- **`fromnum` continuity.** Meshtastic clients treat FROMNUM as a monotonic
  notification counter. Resuming it (rather than resetting to 0) is the more
  correct behavior for a client that never noticed the drop; verify the client
  tolerates a counter that resumes mid-stream.

- **Eviction.** With `MAX_KNOWN_PHONES == 8` and LRU eviction, the 9th distinct
  phone evicts the least-recently-seen. An evicted phone simply falls back to the
  full-handshake path — correct, just not optimized. `last_seen_ms` uses
  `k_uptime_get()` (not available in the pure-C `proxy_protocol.c`, so this stays
  in `ble_gatt.c`).

- **Trust.** `proxy_id` is *claimed* by the phone via NODE_REG; it is not
  authenticated. A phone that claims another phone's id would inherit its
  `fromnum`/config state. This is the same trust model the router already uses
  for DST_ID routing, but it should be stated explicitly.

## Files touched (when implemented)

| File | Change |
|------|--------|
| `src/ble_gatt.c` | add `phone_registry[]`, 4 helpers, 2 call-site edits |
| `src/ble_gatt.h` | none required (all additions are file-static) |
