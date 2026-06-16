/*
 * router.c — FromRadio dispatch: UART bytes → correct BLE connection(s)
 *
 * Two-tier routing:
 *
 * Tier 1 — Meshtastic variant level (which_payload_variant):
 *   Non-packet variants (NodeInfo, MyInfo, Channel, Config, ConfigComplete…)
 *   are always broadcast.  Every phone needs the full config handshake.
 *
 * Tier 2 — Application level (portnum + proxy header):
 *
 *   portnum == PROXY_PORTNUM (256):
 *     Parse the proxy header from Data.payload.bytes.
 *     Route the FULL raw FromRadio bytes to the BLE connection whose
 *     registered proxy_id matches DST_ID in the header.
 *     Fallback to broadcast if:
 *       - Payload too short to contain a valid header
 *       - No connection has registered the target DST_ID yet
 *
 *   portnum != PROXY_PORTNUM (standard Meshtastic traffic):
 *     Broadcast to all connections.
 *     The official Meshtastic app handles standard portnums (TEXT, POSITION,
 *     TELEMETRY, etc.) and the phone's app filters what it cares about.
 *
 *   Packet not decoded (encrypted):
 *     Broadcast raw bytes (cannot inspect portnum).
 */

#include "router.h"
#include "ble_gatt.h"
#include "proxy_protocol.h"

#include "meshtastic/mesh.pb.h"   /* meshtastic_FromRadio_packet_tag */
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(router, LOG_LEVEL_DBG);

void router_dispatch(const uint8_t *raw_bytes, uint16_t len,
                     const struct fromradio_info *info)
{
    /* ----------------------------------------------------------------
     * Tier 1: non-packet FromRadio variant → broadcast unconditionally
     * ---------------------------------------------------------------- */
    if (info->which_variant != meshtastic_FromRadio_packet_tag) {
        LOG_DBG("variant %d (config/meta) → broadcast", info->which_variant);
        ble_gatt_broadcast_fromradio(raw_bytes, len);
        return;
    }

    /* ----------------------------------------------------------------
     * Packet variant — check if it's decoded (plaintext) or encrypted.
     * Encrypted packets cannot be inspected; broadcast as fallback.
     * ---------------------------------------------------------------- */
    if (!info->is_decoded) {
        LOG_DBG("0x%08x→0x%08x encrypted → broadcast",
                info->packet_from, info->packet_to);
        ble_gatt_broadcast_fromradio(raw_bytes, len);
        return;
    }

    /* ----------------------------------------------------------------
     * Tier 2: decoded packet — route by portnum
     * ---------------------------------------------------------------- */
    if (info->portnum == PROXY_PORTNUM) {
        /* Proxy protocol: route by DST_ID embedded in the payload header. */
        struct proxy_header hdr;

        if (!proxy_header_parse(info->payload_bytes, info->payload_len, &hdr)) {
            LOG_WRN("PROXY_PORTNUM: bad header (len=%u) — broadcast fallback",
                    info->payload_len);
            ble_gatt_broadcast_fromradio(raw_bytes, len);
            return;
        }

        struct bt_conn *conn = ble_gatt_get_conn_by_proxy_id(&hdr.dst);
        if (conn != NULL) {
            LOG_DBG("proxy: src=\"%.16s\" dst=\"%.16s\" → targeted (%u B)",
                    (const char *)hdr.src.bytes,
                    (const char *)hdr.dst.bytes,
                    len);
            ble_gatt_enqueue_fromradio(conn, raw_bytes, len);
        } else {
            /*
             * No phone has registered this DST_ID yet (or it's not connected).
             * Broadcast so the message isn't silently dropped. Once the target
             * phone connects and calls NODE_REG, subsequent messages are targeted.
             */
            LOG_DBG("proxy: dst=\"%.16s\" not registered → broadcast fallback",
                    (const char *)hdr.dst.bytes);
            ble_gatt_broadcast_fromradio(raw_bytes, len);
        }
        return;
    }

    /* Standard Meshtastic portnum (TEXT_MESSAGE, POSITION, TELEMETRY, etc.)
     * → broadcast to all connected phones. The app filters what it needs. */
    LOG_DBG("portnum=%u from=0x%08x → broadcast (standard traffic)",
            info->portnum, info->packet_from);
    ble_gatt_broadcast_fromradio(raw_bytes, len);
}
