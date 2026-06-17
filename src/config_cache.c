/*
 * config_cache.c — packed contiguous store of the node's config burst.
 *
 * Layout
 * ------
 *   s_arena[CONFIG_CACHE_ARENA_BYTES]   one byte buffer, frames packed
 *                                       back-to-back, NO per-frame padding.
 *   s_index[CONFIG_CACHE_MAX_FRAMES]    {offset, len, variant} per stored frame.
 *   s_used                              bytes consumed in the arena so far.
 *   s_count                             number of index entries used.
 *   s_ready (atomic_t)                  publish flag (see barrier note below).
 *   s_queuestatus_idx                   index of the cached queueStatus frame,
 *                                       or -1 if none was stored.
 *
 * Threading / memory model
 * ------------------------
 * NO mutexes. All writers (cache_begin / cache_add_frame / cache_mark_ready)
 * run on the single Zephyr system work queue (UART RX → upstream_session), so
 * they never race. Publication uses a Zephyr atomic_t:
 *
 *   - cache_mark_ready() does atomic_set(&s_ready, 1). atomic_set is a
 *     release-store: all prior plain writes (every arena byte, every s_index
 *     entry, s_used, s_count, s_queuestatus_idx) are visible to any thread
 *     that subsequently observes the new value of s_ready.
 *   - cache_is_ready() does atomic_get(&s_ready), an acquire-load: once it
 *     returns true, the reader is guaranteed to see the fully-written arena.
 *
 * This is the ONLY synchronization the cache relies on; it is correct even if a
 * future reader is moved off the system work queue (e.g. a BLE TX thread)
 * without adding a mutex. See config_cache.h for the full rationale.
 */

#include "config_cache.h"

#include <string.h>
#include <errno.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include "meshtastic/mesh.pb.h"   /* meshtastic_FromRadio_queueStatus_tag */

LOG_MODULE_REGISTER(config_cache, LOG_LEVEL_INF);

static uint8_t                s_arena[CONFIG_CACHE_ARENA_BYTES];
static struct cache_frame_ref s_index[CONFIG_CACHE_MAX_FRAMES];
static uint16_t               s_used;
static uint16_t               s_count;
static atomic_t               s_ready;
static int                    s_queuestatus_idx = -1;
static int                    s_own_nodeinfo_idx = -1;  /* first node_info = OWN */

void cache_begin(void)
{
    /* Reset for a fresh burst. Clear ready FIRST so no reader can observe a
     * stale "ready" while we are mid-rewrite. */
    atomic_set(&s_ready, 0);
    s_used             = 0;
    s_count            = 0;
    s_queuestatus_idx  = -1;
    s_own_nodeinfo_idx = -1;
    LOG_INF("cache_begin: arena=%u B, max_frames=%u",
            (unsigned)CONFIG_CACHE_ARENA_BYTES,
            (unsigned)CONFIG_CACHE_MAX_FRAMES);
}

int cache_add_frame(const uint8_t *raw, uint16_t len, int variant)
{
    /* Overflow policy (ADR-001): never silently cap. Reject the frame and
     * LOG_WRN with exact byte counts; the fixed part + first K node_info that
     * already fit remain valid, the rest repopulate via live broadcast. */
    if (s_count >= CONFIG_CACHE_MAX_FRAMES) {
        LOG_WRN("frame cap reached: %u/%u frames, dropping variant=%d (%u B); "
                "remaining frames repopulate via live broadcast",
                (unsigned)s_count, (unsigned)CONFIG_CACHE_MAX_FRAMES,
                variant, (unsigned)len);
        return -ENOMEM;
    }

    if ((uint32_t)s_used + (uint32_t)len > CONFIG_CACHE_ARENA_BYTES) {
        LOG_WRN("arena overflow: used=%u B + frame=%u B > arena=%u B, "
                "dropping variant=%d; remaining frames repopulate via live "
                "broadcast",
                (unsigned)s_used, (unsigned)len,
                (unsigned)CONFIG_CACHE_ARENA_BYTES, variant);
        return -ENOMEM;
    }

    memcpy(&s_arena[s_used], raw, len);
    s_index[s_count].offset  = s_used;
    s_index[s_count].len     = len;
    s_index[s_count].variant = variant;

    /* Remember the queueStatus frame for fast lookup by cache_get_queuestatus.
     * Keep the last one seen (the node emits at most one in the burst). */
    if (variant == meshtastic_FromRadio_queueStatus_tag) {
        s_queuestatus_idx = (int)s_count;
    }

    /* Remember the FIRST node_info: it is the node's OWN NodeInfo (the burst
     * always sends own node_info before the other-node node_infos). Used to
     * segment replays for the special want_config nonces. */
    if (variant == meshtastic_FromRadio_node_info_tag && s_own_nodeinfo_idx < 0) {
        s_own_nodeinfo_idx = (int)s_count;
    }

    LOG_DBG("cache_add_frame: idx=%u variant=%d len=%u (used=%u B)",
            (unsigned)s_count, variant, (unsigned)len,
            (unsigned)(s_used + len));

    s_used  += len;
    s_count += 1;
    return 0;
}

void cache_mark_ready(void)
{
    /* Release-store: publishes every preceding arena/index write. See header. */
    atomic_set(&s_ready, 1);
    LOG_INF("cache_mark_ready: %u frames, %u B published",
            (unsigned)s_count, (unsigned)s_used);
}

bool cache_is_ready(void)
{
    /* Acquire-load: pairs with the release-store in cache_mark_ready(). */
    return atomic_get(&s_ready) != 0;
}

const struct cache_frame_ref *cache_frame_at(uint16_t idx)
{
    if (idx >= s_count) {
        return NULL;
    }
    return &s_index[idx];
}

const uint8_t *cache_frame_bytes(const struct cache_frame_ref *ref)
{
    if (ref == NULL) {
        return NULL;
    }
    return &s_arena[ref->offset];
}

uint16_t cache_frame_count(void)
{
    return s_count;
}

bool cache_get_queuestatus(const uint8_t **out_buf, uint16_t *out_len)
{
    if (s_queuestatus_idx < 0) {
        return false;
    }
    const struct cache_frame_ref *ref = &s_index[s_queuestatus_idx];
    *out_buf = &s_arena[ref->offset];
    *out_len = ref->len;
    return true;
}

bool cache_frame_in_segment(uint16_t idx, uint32_t nonce)
{
    if (idx >= s_count) {
        return false;
    }

    int variant = s_index[idx].variant;

    /* config_complete_id is the terminator — synthesized per phone, never
     * replayed from the cache (would carry the boot nonce). */
    if (variant == meshtastic_FromRadio_config_complete_id_tag) {
        return false;
    }

    bool is_nodeinfo = (variant == meshtastic_FromRadio_node_info_tag);

    if (nonce == MESH_NONCE_ONLY_NODES) {
        /* ONLY_NODES (69421): node_info frames only (own + others). */
        return is_nodeinfo;
    }

    if (nonce == MESH_NONCE_ONLY_CONFIG) {
        /* ONLY_CONFIG (69420): everything except other-node node_info; keep the
         * OWN node_info (the first one). */
        if (is_nodeinfo && (int)idx != s_own_nodeinfo_idx) {
            return false;
        }
        return true;
    }

    /* Normal nonce: replay the full burst. */
    return true;
}
