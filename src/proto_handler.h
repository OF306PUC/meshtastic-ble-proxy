#ifndef PROTO_HANDLER_H
#define PROTO_HANDLER_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Routing information extracted from a decoded FromRadio message.
 *
 * payload_bytes / payload_len are valid only when:
 *   which_variant == meshtastic_FromRadio_packet_tag  AND
 *   is_decoded    == true
 *
 * payload_bytes points into proto_handler's internal static buffer.
 * It remains valid until the next call to proto_decode_fromradio().
 * Since all processing is single-threaded (system work queue), this is safe.
 */
struct fromradio_info {
    int       which_variant;    /* meshtastic_FromRadio_*_tag             */
    uint32_t  packet_from;      /* MeshPacket.from                        */
    uint32_t  packet_to;        /* MeshPacket.to                          */
    bool      is_decoded;       /* MeshPacket variant == decoded (not encrypted) */
    uint32_t  portnum;          /* Data.portnum  — valid when is_decoded   */
    const uint8_t *payload_bytes; /* Data.payload.bytes — valid when is_decoded */
    uint16_t  payload_len;      /* Data.payload.size  — valid when is_decoded */
};

/*
 * Decode raw FromRadio protobuf bytes and populate routing fields.
 *
 * Uses a module-level static for the decoded struct (~2 KB) — must not be
 * called concurrently from multiple threads.
 *
 * @return 0 on success, -EINVAL on nanopb decode failure.
 */
int proto_decode_fromradio(const uint8_t *buf, uint16_t len,
                           struct fromradio_info *out);

/*
 * Information extracted from a decoded ToRadio message (phone -> radio).
 *
 * The proxy only cares about two control variants on the BLE write path:
 *   - want_config_id : the phone is requesting the config burst; the proxy
 *                      serves this locally from the cache (never forwards to
 *                      UART). The nonce is echoed back in config_complete_id.
 *   - heartbeat      : serial-link keepalive; absorbed locally (reactive
 *                      queueStatus reply), never forwarded to UART.
 *
 * Any other variant (notably `packet`) leaves both has_* flags false and is
 * forwarded to UART as today.
 */
struct toradio_info {
    bool      has_want_config;  /* which_payload_variant == want_config_id_tag */
    uint32_t  want_config_id;   /* the phone's config nonce — valid iff above  */
    bool      has_heartbeat;    /* which_payload_variant == heartbeat_tag      */
};

/*
 * Decode raw ToRadio protobuf bytes (phone -> radio) and populate out.
 *
 * Uses a module-level static for the decoded struct (~2 KB) — separate from
 * the FromRadio static — must not be called concurrently from multiple
 * threads. Safe here because all decode/cache processing runs on the single
 * system work queue (see proto_handler.c).
 *
 * @return 0 on success, -EINVAL on nanopb decode failure.
 */
int proto_decode_toradio(const uint8_t *buf, uint16_t len,
                         struct toradio_info *out);

/*
 * Encode a synthetic FromRadio{config_complete_id = nonce} into out_buf.
 *
 * Used by the per-phone replay path to close a phone's config burst with
 * that phone's own nonce. The message is built with nanopb pb_encode against
 * meshtastic_FromRadio_fields — never hand-written tag/varint bytes.
 *
 * @param nonce     the phone's want_config nonce to echo back.
 * @param out_buf   destination buffer.
 * @param buf_size  size of out_buf in bytes.
 * @param out_len   on success, receives the number of bytes written.
 * @return 0 on success, -ENOMEM if out_buf is too small, -EINVAL on encode
 *         failure.
 */
int proto_encode_config_complete(uint32_t nonce, uint8_t *out_buf,
                                 uint16_t buf_size, uint16_t *out_len);

#endif /* PROTO_HANDLER_H */
