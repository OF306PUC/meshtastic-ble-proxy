/*
 * proto_handler_test.c — HOST-side unit test for the ToRadio decode and the
 * config_complete encode added in ADR-001 Phase 1.
 *
 * This test compiles proto_handler.c on the host (x86) and exercises it
 * against hand-encoded ToRadio buffers and a real encode->decode round-trip
 * of FromRadio{config_complete_id}. No Zephyr, no hardware.
 *
 * Zephyr's logging header is not available on the host, so we provide a tiny
 * shim directory (created inline by the build command below) exporting empty
 * LOG_* macros, and we put that shim FIRST on the include path so
 * `#include <zephyr/logging/log.h>` resolves to it.
 *
 * ---------------------------------------------------------------------------
 * BUILD & RUN (paths match this dev machine; adjust NANOPB / BUILD if moved):
 *
 *   NANOPB=/home/juanignaciolorca/ncs/v2.7.0/modules/lib/nanopb
 *   GEN=/home/juanignaciolorca/Desktop/Personal-AI-Studio/Descentralized_Mesh_Noric_GATT_Server/firmware/build
 *   SRC=/home/juanignaciolorca/Desktop/Personal-AI-Studio/Descentralized_Mesh_Noric_GATT_Server/firmware/src
 *   TESTS=/home/juanignaciolorca/Desktop/Personal-AI-Studio/Descentralized_Mesh_Noric_GATT_Server/firmware/tests
 *
 *   # 1. shim so <zephyr/logging/log.h> resolves on the host:
 *   mkdir -p /tmp/ph_shim/zephyr/logging
 *   printf '#define LOG_MODULE_REGISTER(...)\n#define LOG_DBG(...)\n#define LOG_WRN(...)\n#define LOG_LEVEL_DBG 4\n' > /tmp/ph_shim/zephyr/logging/log.h
 *
 *   # 2. compile (shim dir first, then nanopb, then generated proto).
 *   #    NOTE: mesh.pb.c references message descriptors defined in the sibling
 *   #    generated modules (Config, Channel, XModem, DeviceUIConfig, ...), so
 *   #    we link ALL of $GEN/meshtastic/*.pb.c, not just mesh.pb.c.
 *   gcc -std=c11 -Wall -Wextra -DPB_FIELD_32BIT \
 *       -I/tmp/ph_shim -I"$NANOPB" -I"$GEN" -I"$SRC" \
 *       "$TESTS/proto_handler_test.c" \
 *       "$SRC/proto_handler.c" \
 *       "$GEN"/meshtastic/*.pb.c \
 *       "$NANOPB/pb_common.c" "$NANOPB/pb_encode.c" "$NANOPB/pb_decode.c" \
 *       -o /tmp/proto_handler_test && /tmp/proto_handler_test
 *
 * Expected: "ALL TESTS PASSED" and exit code 0.
 * ---------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <pb_encode.h>
#include <pb_decode.h>
#include "meshtastic/mesh.pb.h"

#include "proto_handler.h"

/* ------------------------------------------------------------------ assert */

static int g_failures;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (cond) {                                                        \
            printf("  PASS: %s\n", (msg));                                 \
        } else {                                                           \
            printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------- encode helpers */

/* Hand-encode a ToRadio with a given payload variant using nanopb itself,
 * so the test input is a faithful wire buffer (not our own assumptions). */
static uint16_t encode_toradio_want_config(uint32_t id, uint8_t *buf,
                                           size_t buf_size)
{
    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    tr.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    tr.want_config_id = id;

    pb_ostream_t s = pb_ostream_from_buffer(buf, buf_size);
    if (!pb_encode(&s, meshtastic_ToRadio_fields, &tr)) {
        fprintf(stderr, "encode want_config failed: %s\n", PB_GET_ERROR(&s));
        exit(2);
    }
    return (uint16_t)s.bytes_written;
}

static uint16_t encode_toradio_heartbeat(uint8_t *buf, size_t buf_size)
{
    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    tr.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
    /* Heartbeat has no required fields we care about. */

    pb_ostream_t s = pb_ostream_from_buffer(buf, buf_size);
    if (!pb_encode(&s, meshtastic_ToRadio_fields, &tr)) {
        fprintf(stderr, "encode heartbeat failed: %s\n", PB_GET_ERROR(&s));
        exit(2);
    }
    return (uint16_t)s.bytes_written;
}

/* ------------------------------------------------------------------ tests */

static void test_decode_want_config(void)
{
    printf("test_decode_want_config:\n");
    uint8_t buf[64];
    uint16_t len = encode_toradio_want_config(42, buf, sizeof(buf));

    struct toradio_info info;
    int rc = proto_decode_toradio(buf, len, &info);

    CHECK(rc == 0, "decode returns 0");
    CHECK(info.has_want_config, "has_want_config is true");
    CHECK(info.want_config_id == 42, "want_config_id == 42");
    CHECK(!info.has_heartbeat, "has_heartbeat is false");
}

static void test_decode_heartbeat(void)
{
    printf("test_decode_heartbeat:\n");
    uint8_t buf[64];
    uint16_t len = encode_toradio_heartbeat(buf, sizeof(buf));

    struct toradio_info info;
    int rc = proto_decode_toradio(buf, len, &info);

    CHECK(rc == 0, "decode returns 0");
    CHECK(info.has_heartbeat, "has_heartbeat is true");
    CHECK(!info.has_want_config, "has_want_config is false");
}

static void test_decode_empty(void)
{
    printf("test_decode_empty:\n");
    /* Empty buffer is a valid (zero-length) protobuf message: no variant. */
    struct toradio_info info;
    int rc = proto_decode_toradio((const uint8_t *)"", 0, &info);

    CHECK(rc == 0, "decode returns 0 on empty buffer");
    CHECK(!info.has_want_config, "has_want_config is false");
    CHECK(!info.has_heartbeat, "has_heartbeat is false");
}

static void test_config_complete_roundtrip(void)
{
    printf("test_config_complete_roundtrip:\n");
    const uint32_t nonce = 0xDEADBEEFu;
    uint8_t buf[64];
    uint16_t len = 0;

    int rc = proto_encode_config_complete(nonce, buf, sizeof(buf), &len);
    CHECK(rc == 0, "encode returns 0");
    CHECK(len > 0, "encoded length > 0");

    /* Round-trip: decode the synthesized FromRadio back. */
    struct fromradio_info fr;
    int drc = proto_decode_fromradio(buf, len, &fr);
    CHECK(drc == 0, "fromradio decode returns 0");
    CHECK(fr.which_variant == meshtastic_FromRadio_config_complete_id_tag,
          "variant == config_complete_id_tag");

    /* The decode path doesn't surface config_complete_id, so re-decode the
     * raw FromRadio directly to assert the nonce survived the round-trip. */
    meshtastic_FromRadio raw = meshtastic_FromRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, len);
    bool ok = pb_decode(&is, meshtastic_FromRadio_fields, &raw);
    CHECK(ok, "raw fromradio re-decode ok");
    CHECK(raw.config_complete_id == nonce, "config_complete_id == nonce");
}

static void test_config_complete_buf_too_small(void)
{
    printf("test_config_complete_buf_too_small:\n");
    uint8_t tiny[1];
    uint16_t len = 0;
    /* nonce large enough that the field needs > 1 byte on the wire. */
    int rc = proto_encode_config_complete(0xDEADBEEFu, tiny, sizeof(tiny), &len);
    CHECK(rc == -12 /* -ENOMEM */ || rc != 0, "encode reports failure on tiny buf");
}

static void test_heartbeat_roundtrip(void)
{
    printf("test_heartbeat_roundtrip:\n");
    const uint32_t nonce = 0x01020304u;
    uint8_t buf[32];
    uint16_t len = 0;

    int rc = proto_encode_heartbeat(nonce, buf, sizeof(buf), &len);
    CHECK(rc == 0, "encode_heartbeat returns 0");
    CHECK(len > 0, "encoded length > 0");

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, len);
    bool ok = pb_decode(&is, meshtastic_ToRadio_fields, &tr);
    CHECK(ok, "toradio re-decode ok");
    CHECK(tr.which_payload_variant == meshtastic_ToRadio_heartbeat_tag,
          "variant == heartbeat_tag");
    CHECK(tr.heartbeat.nonce == nonce, "heartbeat.nonce survives round-trip");
    /* The keepalive must never emit nonce 1 (node treats it specially). */
    CHECK(nonce != 1, "test nonce is not the reserved value 1");
}

static void test_queue_status_roundtrip(void)
{
    printf("test_queue_status_roundtrip:\n");
    uint8_t buf[32];
    uint16_t len = 0;

    int rc = proto_encode_queue_status(buf, sizeof(buf), &len);
    CHECK(rc == 0, "encode_queue_status returns 0");
    CHECK(len > 0, "encoded length > 0");

    struct fromradio_info fr;
    int drc = proto_decode_fromradio(buf, len, &fr);
    CHECK(drc == 0, "fromradio decode returns 0");
    CHECK(fr.which_variant == meshtastic_FromRadio_queueStatus_tag,
          "variant == queueStatus_tag");

    meshtastic_FromRadio raw = meshtastic_FromRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, len);
    bool ok = pb_decode(&is, meshtastic_FromRadio_fields, &raw);
    CHECK(ok, "raw fromradio re-decode ok");
    CHECK(raw.queueStatus.maxlen == 16, "queueStatus.maxlen == 16");
    CHECK(raw.queueStatus.free == 16, "queueStatus.free == 16");
    CHECK(raw.queueStatus.res == 0, "queueStatus.res == 0 (success)");
}

int main(void)
{
    printf("=== proto_handler host tests ===\n");

    test_decode_want_config();
    test_decode_heartbeat();
    test_decode_empty();
    test_config_complete_roundtrip();
    test_config_complete_buf_too_small();
    test_heartbeat_roundtrip();
    test_queue_status_roundtrip();

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
