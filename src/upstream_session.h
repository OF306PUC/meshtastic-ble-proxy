#ifndef UPSTREAM_SESSION_H
#define UPSTREAM_SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/bluetooth/conn.h>   /* struct bt_conn — BT stack, not ble_gatt */

#include "proto_handler.h"           /* struct fromradio_info */

/*
 * upstream_session — the proxy's single config session with the node.
 *
 * At boot the proxy issues its OWN want_config (its own nonce) to the node over
 * UART, consumes the ordered FromRadio burst into config_cache, and on the
 * matching config_complete_id marks the cache ready and goes LIVE. Phones that
 * connect before the cache is ready are queued PENDING and served once ready.
 * See ADR-001 (State machines, Module plan, Phase 0).
 *
 * Threading: all entry points run on the single Zephyr system work queue (the
 * UART RX callback path and the BLE callback path are both work-queue context).
 * No mutexes; state is a plain enum mutated only from that single context.
 *
 * NOTE: this module has NO dependency on ble_gatt. It only references
 * `struct bt_conn` (an opaque BT-stack type) and stubs the per-phone replay
 * (Task C wires the real replay into ble_gatt).
 */

enum upstream_state {
    UPSTREAM_BOOT,         /* nothing started yet                            */
    UPSTREAM_FETCHING,     /* want_config sent; consuming the burst → cache  */
    UPSTREAM_CACHE_READY,  /* burst complete, cache published (transient)    */
    UPSTREAM_LIVE,         /* normal routing; FromRadio no longer cached     */
};

/*
 * Kick off the boot fetch: pick a nonce, encode a ToRadio{want_config_id}, send
 * it over UART, call cache_begin(), and move to UPSTREAM_FETCHING.
 *
 * The nonce is sys_rand32_get(), forced to be neither 0 nor 1: nonce 1 is
 * special on the node — it forces a NodeInfo rebroadcast — so it must never be
 * used as a plain config nonce. If the RNG yields 0 or 1 it is bumped to 2.
 *
 * Must be called after uart_meshtastic_init() (so TX can be enqueued).
 */
void upstream_session_start(void);

/* Current upstream state. */
enum upstream_state upstream_get_state(void);

/*
 * Feed one decoded FromRadio frame to the upstream state machine.
 *
 * @param payload  raw FromRadio protobuf bytes (no serial header).
 * @param len      payload length.
 * @param info     already-decoded routing info for this frame.
 *
 * @return true  if the frame was CONSUMED by the session (added to the cache or
 *               it was the closing config_complete_id) — the caller must NOT
 *               broadcast it.
 *         false if the caller still owns the frame (live traffic / packet
 *               variants, or anything received while LIVE) — the caller routes
 *               it as usual.
 */
bool upstream_on_fromradio(const uint8_t *payload, uint16_t len,
                           const struct fromradio_info *info);

/*
 * Register a phone that asked for config before the cache was ready. It is
 * remembered as PENDING and served (replayed) once the cache becomes ready.
 * Safe to call repeatedly with the same conn (deduplicated).
 */
void upstream_on_pending_phone(struct bt_conn *conn);

#endif /* UPSTREAM_SESSION_H */
