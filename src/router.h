#ifndef ROUTER_H
#define ROUTER_H

#include <stdint.h>
#include "proto_handler.h"

/*
 * Dispatch a raw FromRadio payload to the appropriate BLE connection(s).
 *
 * Routing rules (in priority order):
 *
 *  1. Non-packet variants (NodeInfo, MyInfo, Channel, Config, ConfigComplete…)
 *     → broadcast to all connections.
 *     Rationale: every phone needs the full config handshake to complete.
 *
 *  2. Encrypted packets (MeshPacket.which_payload_variant != decoded_tag)
 *     → broadcast to all connections (payload cannot be inspected).
 *
 *  3. Decoded packet with portnum == PROXY_PORTNUM (256):
 *     Parse the proxy header from Data.payload.bytes.
 *     Route the raw FromRadio bytes to the BLE connection whose registered
 *     proxy_id matches DST_ID in the header.
 *     Fallback to broadcast if the header is malformed or no connection has
 *     registered that DST_ID yet (guarantees no silent drops).
 *
 *  4. Decoded packet with any other portnum (TEXT_MESSAGE, POSITION, etc.)
 *     → broadcast to all connections.
 *     The phone app filters what it cares about.
 *
 * @param raw_bytes  Protobuf bytes as received from UART (no framing header).
 * @param len        Byte count.
 * @param info       Pre-decoded routing fields from proto_decode_fromradio().
 */
void router_dispatch(const uint8_t *raw_bytes, uint16_t len,
                     const struct fromradio_info *info);

#endif /* ROUTER_H */
