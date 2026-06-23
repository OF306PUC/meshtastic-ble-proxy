#ifndef CONFIG_CACHE_H
#define CONFIG_CACHE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * config_cache — boot-populated, read-only-after-ready store of the node's
 * config burst (the ordered FromRadio frames the node emits in response to a
 * want_config).
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
 * in ble_gatt) also run on the system work queue today — but
 * cache_mark_ready() still publishes via a Zephyr atomic_t store, and
 * cache_is_ready() reads it via an atomic load. Those atomics carry the
 * acquire/release barrier that guarantees: any reader that observes
 * cache_is_ready() == true also observes every byte written into the arena and
 * every index entry before cache_mark_ready() was called. This makes the cache
 * correct even if a future reader is moved to another context (e.g. a BLE TX
 * thread) without adding a mutex.
 */

/* Default arena size — 4 KB. RAM budget sizes this from the real
 * node's Phase 0 boot measurement; upstream_session logs the observed total.
 * Update this define once Phase 0 has been measured. */
#define CONFIG_CACHE_ARENA_BYTES 4096  /* 4 KB default; update after Phase 0 measurement */

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
 * Overflow policy: if appending this frame
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

/*
 * Return a read-only pointer to the raw bytes of the frame described by ref.
 *
 * The arena is private to config_cache; per-phone replay (ble_gatt) holds only
 * a cache_frame_ref (from cache_frame_at) and resolves it to bytes here, so the
 * replay path reads STRAIGHT FROM THE SHARED ARENA — no per-connection copy of
 * the burst. The pointer is valid for the cache lifetime
 * (read-only after cache_mark_ready). Returns NULL if ref is NULL.
 */
const uint8_t *cache_frame_bytes(const struct cache_frame_ref *ref);

/* Number of frames currently stored. */
uint16_t cache_frame_count(void);

/*
 * Meshtastic special want_config nonces (PhoneAPI). The real phone app fetches
 * the node in TWO rounds with these sentinel nonces (confirmed against a real
 * node, .tmp/burst_*.hex, and NimbleBluetooth.cpp). See memory
 * meshtastic-want-config-special-nonces.
 */
#define MESH_NONCE_ONLY_CONFIG 69420u  /* config + OWN node_info; skip other nodes */
#define MESH_NONCE_ONLY_NODES  69421u  /* node_info only (own + others)            */

/*
 * Whether cached frame `idx` should be replayed for a phone that sent `nonce`.
 *
 *   MESH_NONCE_ONLY_NODES (69421): only node_info frames.
 *   MESH_NONCE_ONLY_CONFIG (69420): everything EXCEPT non-first node_info
 *       (keep the OWN node_info — the first one — drop other-node node_info).
 *   any other nonce: the full burst (true for every frame).
 *
 * A config_complete_id frame is NEVER served from the cache (the replay
 * synthesizes it per phone), so this returns false for that variant in all
 * segments. Returns false for out-of-range idx.
 */
bool cache_frame_in_segment(uint16_t idx, uint32_t nonce);

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
