/*
 * upstream_session.c — the proxy's single config session with the node.
 *
 * State machine (ADR-001):
 *
 *   BOOT ──start()──▶ FETCHING ──(config_complete_id == our nonce)──▶
 *                                  CACHE_READY ─(serve pending)─▶ LIVE
 *
 *   FETCHING:
 *     - non-packet FromRadio variant  → cache_add_frame(), CONSUMED (true)
 *     - config_complete_id == nonce   → cache_mark_ready(), serve pending,
 *                                        CACHE_READY → LIVE, CONSUMED (true)
 *     - packet variant                → NOT consumed (false): live mesh
 *                                        traffic is broadcast immediately,
 *                                        never held in the config cache.
 *   LIVE:
 *     - everything                    → NOT consumed (false): normal routing.
 *
 * Phase 0 instrumentation (MANDATORY hard gate, ADR-001):
 *   per-variant byte/frame counters are accumulated during FETCHING and dumped
 *   at cache_mark_ready() time, so CONFIG_CACHE_ARENA_BYTES can be sized from
 *   the real node. See log_phase0_breakdown().
 *
 * Threading: every entry point runs on the single Zephyr system work queue
 * (UART RX callback path and BLE callback path are both work-queue context).
 * No mutexes; s_state and the pending array are mutated only from that single
 * context. The nonce/counters are likewise single-writer.
 *
 * ble_gatt dependency: NONE. The per-phone replay that serves a pending phone
 * is STUBBED here (TODO Task C). This module references only `struct bt_conn`
 * (an opaque BT-stack type), never any ble_gatt symbol.
 */

#include "upstream_session.h"
#include "config_cache.h"
#include "uart_meshtastic.h"
#include "proto_handler.h"

#include <string.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>      /* ARRAY_SIZE, ARG_UNUSED */
#include <zephyr/logging/log.h>

#include "meshtastic/mesh.pb.h"   /* meshtastic_ToRadio_*, meshtastic_FromRadio_*_tag */

LOG_MODULE_REGISTER(upstream_session, LOG_LEVEL_INF);

/* pb_encode for the want_config ToRadio; pb_decode for the config_complete
 * nonce check (fromradio_info does not carry the nonce). */
#include <pb_encode.h>
#include <pb_decode.h>

/* -------------------------------------------------------------------------
 * Module state (single-writer, system work queue — see file header).
 * ------------------------------------------------------------------------- */
static enum upstream_state s_state = UPSTREAM_BOOT;
static uint32_t            s_nonce;

/* Pending phones: connected and asked for config before CACHE_READY. Served
 * once the cache is ready. Sized to MAX_BLE_CONNECTIONS (== CONFIG_BT_MAX_CONN)
 * without pulling in ble_gatt.h. */
static struct bt_conn *s_pending[CONFIG_BT_MAX_CONN];
static uint8_t         s_pending_count;

/* -------------------------------------------------------------------------
 * Phase 0 per-variant accumulators (bytes + counts). Reset in cache_begin
 * path (upstream_session_start) and dumped at mark_ready.
 * ------------------------------------------------------------------------- */
struct phase0_stats {
    uint32_t total_bytes;
    uint32_t my_info_bytes;
    uint32_t deviceuiConfig_bytes;
    uint32_t node_info_bytes;
    uint16_t node_info_count;
    uint32_t metadata_bytes;
    uint32_t channel_bytes;
    uint16_t channel_count;
    uint32_t config_bytes;
    uint16_t config_count;
    uint32_t moduleConfig_bytes;
    uint16_t moduleConfig_count;
    uint32_t fileInfo_bytes;
    uint16_t fileInfo_count;
    uint32_t queueStatus_bytes;
    uint16_t queueStatus_count;
    uint32_t config_complete_id;   /* the closing nonce, for the record       */
    uint32_t other_bytes;          /* any variant not enumerated above        */
    uint16_t other_count;
};
static struct phase0_stats s_p0;

static void phase0_reset(void)
{
    memset(&s_p0, 0, sizeof(s_p0));
}

static void phase0_account(int variant, uint16_t len)
{
    s_p0.total_bytes += len;

    switch (variant) {
    case meshtastic_FromRadio_my_info_tag:
        s_p0.my_info_bytes += len;
        break;
    case meshtastic_FromRadio_deviceuiConfig_tag:
        s_p0.deviceuiConfig_bytes += len;
        break;
    case meshtastic_FromRadio_node_info_tag:
        s_p0.node_info_bytes += len;
        s_p0.node_info_count += 1;
        break;
    case meshtastic_FromRadio_metadata_tag:
        s_p0.metadata_bytes += len;
        break;
    case meshtastic_FromRadio_channel_tag:
        s_p0.channel_bytes += len;
        s_p0.channel_count += 1;
        break;
    case meshtastic_FromRadio_config_tag:
        s_p0.config_bytes += len;
        s_p0.config_count += 1;
        break;
    case meshtastic_FromRadio_moduleConfig_tag:
        s_p0.moduleConfig_bytes += len;
        s_p0.moduleConfig_count += 1;
        break;
    case meshtastic_FromRadio_fileInfo_tag:
        s_p0.fileInfo_bytes += len;
        s_p0.fileInfo_count += 1;
        break;
    case meshtastic_FromRadio_queueStatus_tag:
        s_p0.queueStatus_bytes += len;
        s_p0.queueStatus_count += 1;
        break;
    default:
        s_p0.other_bytes += len;
        s_p0.other_count += 1;
        break;
    }
}

static void log_phase0_breakdown(void)
{
    LOG_INF("=== Phase 0 config-burst measurement ===");
    LOG_INF("  my_info:        %u B", (unsigned)s_p0.my_info_bytes);
    LOG_INF("  deviceuiConfig: %u B", (unsigned)s_p0.deviceuiConfig_bytes);
    LOG_INF("  node_info:      %u frames, %u B",
            (unsigned)s_p0.node_info_count, (unsigned)s_p0.node_info_bytes);
    LOG_INF("  metadata:       %u B", (unsigned)s_p0.metadata_bytes);
    LOG_INF("  channel:        %u frames, %u B",
            (unsigned)s_p0.channel_count, (unsigned)s_p0.channel_bytes);
    LOG_INF("  config:         %u frames, %u B",
            (unsigned)s_p0.config_count, (unsigned)s_p0.config_bytes);
    LOG_INF("  moduleConfig:   %u frames, %u B",
            (unsigned)s_p0.moduleConfig_count, (unsigned)s_p0.moduleConfig_bytes);
    LOG_INF("  fileInfo:       %u frames, %u B",
            (unsigned)s_p0.fileInfo_count, (unsigned)s_p0.fileInfo_bytes);
    LOG_INF("  queueStatus:    %u frames, %u B",
            (unsigned)s_p0.queueStatus_count, (unsigned)s_p0.queueStatus_bytes);
    if (s_p0.other_count) {
        LOG_INF("  other:          %u frames, %u B",
                (unsigned)s_p0.other_count, (unsigned)s_p0.other_bytes);
    }
    LOG_INF("  config_complete_id: %u", (unsigned)s_p0.config_complete_id);
    LOG_INF("  TOTAL: %u B", (unsigned)s_p0.total_bytes);
    LOG_INF("Phase 0: burst measured. Set CONFIG_CACHE_ARENA_BYTES >= %u. "
            "Update ADR-001.", (unsigned)s_p0.total_bytes);
}

/* -------------------------------------------------------------------------
 * Pending-phone bookkeeping.
 * ------------------------------------------------------------------------- */
static void serve_one_pending(struct bt_conn *conn)
{
    /*
     * TODO(Task C): replay the cached burst to `conn` via the per-phone replay
     * path in ble_gatt (cursor over config_cache, then a synthesized
     * FromRadio{config_complete_id = that phone's nonce}). Stubbed here so
     * upstream_session compiles with NO ble_gatt dependency.
     */
    ARG_UNUSED(conn);
    LOG_DBG("serve_one_pending: stub (Task C wires ble_gatt replay)");
}

static void serve_pending_phones(void)
{
    if (s_pending_count == 0) {
        return;
    }
    LOG_INF("serving %u pending phone(s) from ready cache",
            (unsigned)s_pending_count);
    for (uint8_t i = 0; i < s_pending_count; i++) {
        serve_one_pending(s_pending[i]);
        s_pending[i] = NULL;
    }
    s_pending_count = 0;
}

/*
 * Decode just the config_complete_id nonce out of a raw FromRadio frame.
 *
 * fromradio_info (proto_handler) does not expose this field, and we must not
 * modify proto_handler, so we run a minimal local nanopb decode into a
 * module-level static FromRadio. Safe single-threaded (system work queue, see
 * file header). Returns false on decode failure or if the frame is not a
 * config_complete_id variant.
 */
static meshtastic_FromRadio s_cc_scratch;   /* ~2 KB — too large for the stack */

static bool decode_config_complete_nonce(const uint8_t *payload, uint16_t len,
                                         uint32_t *out_nonce)
{
    memset(&s_cc_scratch, 0, sizeof(s_cc_scratch));
    pb_istream_t stream = pb_istream_from_buffer(payload, len);
    if (!pb_decode(&stream, meshtastic_FromRadio_fields, &s_cc_scratch)) {
        return false;
    }
    if (s_cc_scratch.which_payload_variant !=
        meshtastic_FromRadio_config_complete_id_tag) {
        return false;
    }
    *out_nonce = s_cc_scratch.config_complete_id;
    return true;
}

/* -------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */
void upstream_session_start(void)
{
    /* Pick a nonce that is neither 0 nor 1. nonce 1 is special on the node:
     * it forces a NodeInfo rebroadcast, so it must never be a plain config
     * nonce. Bump 0/1 to 2. */
    s_nonce = sys_rand32_get();
    if (s_nonce == 0 || s_nonce == 1) {
        s_nonce = 2;
    }

    /* Build ToRadio{want_config_id = nonce} entirely via nanopb. */
    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    tr.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    tr.want_config_id        = s_nonce;

    uint8_t  buf[meshtastic_ToRadio_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &tr)) {
        LOG_ERR("want_config pb_encode failed: %s", PB_GET_ERROR(&stream));
        /* Stay in BOOT; nothing was sent. A retry policy belongs to main/QA. */
        return;
    }

    cache_begin();
    phase0_reset();
    s_pending_count = 0;

    int rc = uart_meshtastic_tx(buf, (uint16_t)stream.bytes_written);
    if (rc != 0) {
        LOG_ERR("want_config tx failed: %d", rc);
        /* Stay in BOOT so a higher layer can retry the start. */
        return;
    }

    s_state = UPSTREAM_FETCHING;
    LOG_INF("upstream start: want_config nonce=%u sent (%u B), FETCHING",
            (unsigned)s_nonce, (unsigned)stream.bytes_written);
}

enum upstream_state upstream_get_state(void)
{
    return s_state;
}

bool upstream_on_fromradio(const uint8_t *payload, uint16_t len,
                           const struct fromradio_info *info)
{
    /* LIVE (and BOOT before a fetch) — the session does not consume frames. */
    if (s_state != UPSTREAM_FETCHING) {
        return false;
    }

    /* Live mesh traffic during the fetch window: NEVER held in the config
     * cache. Hand it back so the caller broadcasts it immediately. */
    if (info->which_variant == meshtastic_FromRadio_packet_tag) {
        return false;
    }

    /* The closing config_complete_id — finish the burst ONLY if it carries
     * OUR nonce. fromradio_info does not expose config_complete_id, so we
     * decode the nonce here with a minimal local nanopb pass (no change to
     * proto_handler). The proxy is the node's sole want_config client
     * (ADR-001 §Context), but we still match strictly so a stray/duplicated
     * config_complete_id can never prematurely close the burst. */
    if (info->which_variant == meshtastic_FromRadio_config_complete_id_tag) {
        uint32_t got_nonce = 0;
        if (!decode_config_complete_nonce(payload, len, &got_nonce)) {
            /* Malformed — treat as a non-fatal config/meta frame: cache it and
             * keep waiting for a well-formed close. */
            LOG_WRN("config_complete decode failed; caching, still FETCHING");
            phase0_account(info->which_variant, len);
            (void)cache_add_frame(payload, len, info->which_variant);
            return true;
        }

        if (got_nonce != s_nonce) {
            /* Not ours. Cache it (preserves burst order) but do NOT finish. */
            LOG_WRN("config_complete nonce=%u != ours=%u — not closing burst",
                    (unsigned)got_nonce, (unsigned)s_nonce);
            phase0_account(info->which_variant, len);
            (void)cache_add_frame(payload, len, info->which_variant);
            return true;
        }

        /* Ours: cache it (part of the replayed burst order), record for the
         * Phase 0 log, publish the cache and go LIVE. */
        phase0_account(info->which_variant, len);
        (void)cache_add_frame(payload, len, info->which_variant);
        s_p0.config_complete_id = got_nonce;

        cache_mark_ready();
        log_phase0_breakdown();

        s_state = UPSTREAM_CACHE_READY;
        serve_pending_phones();
        s_state = UPSTREAM_LIVE;
        LOG_INF("upstream: config_complete nonce=%u → CACHE_READY → LIVE",
                (unsigned)got_nonce);
        return true;
    }

    /* Any other non-packet variant (my_info, node_info, config, …): cache it. */
    phase0_account(info->which_variant, len);
    int rc = cache_add_frame(payload, len, info->which_variant);
    if (rc != 0) {
        /* Overflow already logged with byte counts by config_cache. The frame
         * is dropped (repopulates via live broadcast) but still CONSUMED here:
         * it must NOT be broadcast during the fetch window. */
        LOG_WRN("variant=%d dropped from cache (rc=%d)",
                info->which_variant, rc);
    }
    return true;
}

void upstream_on_pending_phone(struct bt_conn *conn)
{
    if (conn == NULL) {
        return;
    }

    /* If the cache is already ready, serve immediately rather than queueing. */
    if (cache_is_ready()) {
        LOG_DBG("pending phone but cache already ready → serve now");
        serve_one_pending(conn);
        return;
    }

    /* Deduplicate. */
    for (uint8_t i = 0; i < s_pending_count; i++) {
        if (s_pending[i] == conn) {
            return;
        }
    }

    if (s_pending_count >= ARRAY_SIZE(s_pending)) {
        /* Should be impossible: pending count is bounded by live connections,
         * which is bounded by CONFIG_BT_MAX_CONN. Log instead of overflowing. */
        LOG_WRN("pending phone overflow (%u) — dropping registration",
                (unsigned)s_pending_count);
        return;
    }

    s_pending[s_pending_count++] = conn;
    LOG_INF("phone queued PENDING (%u total) until cache ready",
            (unsigned)s_pending_count);
}
