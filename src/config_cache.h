#ifndef CONFIG_CACHE_H
#define CONFIG_CACHE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * config_cache — boot-populated, read-only-after-ready store of the node's
 * config burst (the ordered FromRadio frames the node emits in response to a
 * want_config). See ADR-001 (Module plan, RAM budget, Phase 0).
 *
 * The arena is a single packed contiguous byte buffer: frames are stored
 * back-to-back with NO per-frame padding (the per-frame 512 B cap is a
 * transport limit, not a storage layout). An index array of
 * {offset, len, variant} records where each frame lives.
 *
 * Lifecycle (driven by upstream_session):
 *   cache_begin()                  reset arena + index (UPSTREAM_FETCHING start)
 *   cache_add_frame(raw,len,var)   append one FromRadio frame, in burst order
 *   cache_mark_ready()             publish: arena is now immutable
 *   cache_is_ready()               readers gate on this before touching frames
 *   cache_frame_at(idx)/count()    iterate frames for per-phone replay
 *   cache_get_queuestatus()        fetch the cached queueStatus frame, if any
 *
 * Threading / memory model
 * ------------------------
 * There are NO mutexes in this module. All writes (cache_begin / cache_add_frame
 * / cache_mark_ready) run on the single Zephyr system work queue (the UART RX
 * callback path), so writers never race each other. Readers (per-phone replay
 * in ble_gatt, Task C) also run on the system work queue today — but
 * cache_mark_ready() still publishes via a Zephyr atomic_t store, and
 * cache_is_ready() reads it via an atomic load. Those atomics carry the
 * acquire/release barrier that guarantees: any reader that observes
 * cache_is_ready() == true also observes every byte written into the arena and
 * every index entry before cache_mark_ready() was called. This makes the cache
 * correct even if a future reader is moved to another context (e.g. a BLE TX
 * thread) without adding a mutex.
 */

/* Default arena size — 16 KB. ADR-001 RAM budget sizes this from the real
 * node's Phase 0 boot measurement; upstream_session logs the observed total.
 * Update this define (and ADR-001) once Phase 0 has been measured. */
#define CONFIG_CACHE_ARENA_BYTES 16384  /* 16 KB default; update after Phase 0 measurement */

/* Max number of frames the index can hold. node_info dominates the count
 * (one frame per mesh node). */
#define CONFIG_CACHE_MAX_FRAMES 128

/*
 * One index entry. variant is the meshtastic_FromRadio_*_tag of the frame.
 * raw bytes live at &arena[offset], length len. Read-only after ready.
 */
struct cache_frame_ref {
    uint16_t offset;   /* byte offset into the arena                 */
    uint16_t len;      /* frame length in bytes                       */
    int      variant;  /* meshtastic_FromRadio_*_tag                  */
};

/* Reset the cache for a fresh burst. Clears arena cursor, index and the
 * ready flag. Call at the start of UPSTREAM_FETCHING. */
void cache_begin(void);

/*
 * Append one raw FromRadio frame (protobuf bytes, no serial header) to the
 * arena, in burst order.
 *
 * Overflow policy (ADR-001 — never a silent cap): if appending this frame
 * would exceed CONFIG_CACHE_ARENA_BYTES or CONFIG_CACHE_MAX_FRAMES, the frame
 * is rejected and a LOG_WRN is emitted with exact byte counts. The fixed part
 * of the burst (my_info, deviceuiConfig, metadata, channel, config,
 * moduleConfig, fileInfo) plus the first K node_info frames that DID fit are
 * kept; later node_info frames repopulate via live broadcast. The rejected
 * frame is simply not stored.
 *
 * @return 0 on success, -ENOMEM if the frame did not fit (arena or frame cap).
 */
int cache_add_frame(const uint8_t *raw, uint16_t len, int variant);

/* Publish the cache: store the ready flag with release semantics so any reader
 * that sees cache_is_ready() observes the fully-written arena. */
void cache_mark_ready(void);

/* True once cache_mark_ready() has been called (acquire load). */
bool cache_is_ready(void);

/* Return the index entry for frame idx, or NULL if idx is out of range. */
const struct cache_frame_ref *cache_frame_at(uint16_t idx);

/* Number of frames currently stored. */
uint16_t cache_frame_count(void);

/*
 * Fetch the cached queueStatus frame (variant
 * meshtastic_FromRadio_queueStatus_tag) for BLE liveness replies.
 *
 * @param out_buf  on success, receives a pointer into the arena.
 * @param out_len  on success, receives the frame length.
 * @return true if a queueStatus frame was cached, false otherwise.
 */
bool cache_get_queuestatus(const uint8_t **out_buf, uint16_t *out_len);

#endif /* CONFIG_CACHE_H */
