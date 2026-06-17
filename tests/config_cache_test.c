/*
 * config_cache_test.c — HOST-side unit test for the config_cache arena logic.
 *
 * No Zephyr, no hardware. config_cache.c depends on three Zephyr/meshtastic
 * bits that don't exist on the host:
 *   - <zephyr/sys/atomic.h>   (atomic_t, atomic_set/atomic_get)
 *   - <zephyr/logging/log.h>  (LOG_MODULE_REGISTER, LOG_INF/WRN/DBG, levels)
 *   - "meshtastic/mesh.pb.h"  (only meshtastic_FromRadio_queueStatus_tag)
 *
 * Rather than build the full nanopb/meshtastic tree just to read one #define,
 * we provide a tiny shim directory (created inline by the build command below)
 * for all three headers and then #include config_cache.c directly. Including
 * the .c (instead of linking it) lets the shims defined on the include path
 * substitute for the real Zephyr APIs while exercising the EXACT arena logic
 * under test. config_cache.c/.h are NOT modified.
 *
 * atomic_t shim: a plain `long`. atomic_set/atomic_get are ordinary
 * load/store — fine for a single-threaded host test (the real release/acquire
 * barrier only matters across threads, which this test does not create).
 *
 * ---------------------------------------------------------------------------
 * BUILD & RUN (paths match this dev machine; adjust SRC if moved):
 *
 *   SRC=/home/juanignaciolorca/Desktop/Personal-AI-Studio/Descentralized_Mesh_Noric_GATT_Server/firmware/src
 *   TESTS=/home/juanignaciolorca/Desktop/Personal-AI-Studio/Descentralized_Mesh_Noric_GATT_Server/firmware/tests
 *
 *   # shims so the Zephyr/meshtastic includes resolve on the host:
 *   mkdir -p /tmp/cc_shim/zephyr/sys /tmp/cc_shim/zephyr/logging /tmp/cc_shim/meshtastic
 *   printf 'typedef long atomic_t;\nstatic inline void atomic_set(atomic_t*p,long v){*p=v;}\nstatic inline long atomic_get(const atomic_t*p){return *p;}\n' > /tmp/cc_shim/zephyr/sys/atomic.h
 *   printf '#define LOG_MODULE_REGISTER(...)\n#define LOG_INF(...)\n#define LOG_WRN(...)\n#define LOG_DBG(...)\n#define LOG_LEVEL_INF 3\n' > /tmp/cc_shim/zephyr/logging/log.h
 *   printf '#define meshtastic_FromRadio_queueStatus_tag 11\n' > /tmp/cc_shim/meshtastic/mesh.pb.h
 *
 *   # compile (shim dir first on the include path, then src for config_cache.h):
 *   gcc -std=c11 -Wall -Wextra \
 *       -I/tmp/cc_shim -I"$SRC" \
 *       "$TESTS/config_cache_test.c" \
 *       -o /tmp/config_cache_test && /tmp/config_cache_test
 *
 * Expected: "ALL TESTS PASSED" and exit code 0.
 * ---------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

/* Pull in the module under test directly. The shim headers on the -I path
 * satisfy its Zephyr/meshtastic includes. */
#include "config_cache.c"

/* meshtastic_FromRadio_queueStatus_tag comes from the shim mesh.pb.h (value
 * 11, taken from the generated meshtastic/mesh.pb.h). Keep a local alias so
 * the test reads clearly even if the shim define moves. */
#define QUEUESTATUS_TAG meshtastic_FromRadio_queueStatus_tag

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

/* Fill buf[0..len) with a deterministic, per-frame-distinct pattern so we can
 * assert exact byte round-trip and detect any cross-frame corruption. */
static void fill_pattern(uint8_t *buf, uint16_t len, uint8_t seed)
{
    for (uint16_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + i);
    }
}

static bool check_pattern(const uint8_t *buf, uint16_t len, uint8_t seed)
{
    for (uint16_t i = 0; i < len; i++) {
        if (buf[i] != (uint8_t)(seed + i)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ tests */

/* 1. cache_begin resets count/used; not ready after begin. */
static void test_begin_resets(void)
{
    printf("test_begin_resets:\n");
    /* Dirty the cache first so we know begin actually clears it. */
    uint8_t junk[8];
    fill_pattern(junk, sizeof(junk), 0x55);
    cache_begin();
    (void)cache_add_frame(junk, sizeof(junk), 1);
    cache_mark_ready();

    cache_begin();
    CHECK(cache_frame_count() == 0, "frame count == 0 after begin");
    CHECK(s_used == 0, "arena used bytes == 0 after begin");
    CHECK(!cache_is_ready(), "cache_is_ready() is false after begin");

    const uint8_t *qbuf; uint16_t qlen;
    CHECK(!cache_get_queuestatus(&qbuf, &qlen),
          "queuestatus cleared after begin");
}

/* 2. Frames pack contiguously and round-trip byte-for-byte. */
static void test_pack_and_roundtrip(void)
{
    printf("test_pack_and_roundtrip:\n");
    uint8_t f0[10], f1[20], f2[30];
    fill_pattern(f0, sizeof(f0), 0x10);
    fill_pattern(f1, sizeof(f1), 0x40);
    fill_pattern(f2, sizeof(f2), 0x80);

    cache_begin();
    CHECK(cache_add_frame(f0, sizeof(f0), 1) == 0, "add frame 0 (10 B)");
    CHECK(cache_add_frame(f1, sizeof(f1), 2) == 0, "add frame 1 (20 B)");
    CHECK(cache_add_frame(f2, sizeof(f2), 3) == 0, "add frame 2 (30 B)");

    CHECK(cache_frame_count() == 3, "frame count == 3");

    const struct cache_frame_ref *r0 = cache_frame_at(0);
    const struct cache_frame_ref *r1 = cache_frame_at(1);
    const struct cache_frame_ref *r2 = cache_frame_at(2);
    CHECK(r0 && r1 && r2, "all three refs non-NULL");

    /* Contiguous packing: offsets are the running sum of prior lengths. */
    CHECK(r0->offset == 0  && r0->len == 10, "frame 0: offset 0, len 10");
    CHECK(r1->offset == 10 && r1->len == 20, "frame 1: offset 10, len 20");
    CHECK(r2->offset == 30 && r2->len == 30, "frame 2: offset 30, len 30");

    /* Out-of-range index returns NULL. */
    CHECK(cache_frame_at(3) == NULL, "cache_frame_at(3) == NULL");

    /* Byte round-trip: arena bytes equal what was written, per frame. */
    CHECK(check_pattern(&s_arena[r0->offset], r0->len, 0x10),
          "frame 0 bytes round-trip");
    CHECK(check_pattern(&s_arena[r1->offset], r1->len, 0x40),
          "frame 1 bytes round-trip");
    CHECK(check_pattern(&s_arena[r2->offset], r2->len, 0x80),
          "frame 2 bytes round-trip");
}

/* 3. cache_get_queuestatus returns the cached queueStatus frame when present. */
static void test_get_queuestatus(void)
{
    printf("test_get_queuestatus:\n");
    const uint8_t *qbuf; uint16_t qlen;

    /* No queueStatus frame yet → false. */
    cache_begin();
    (void)cache_add_frame((const uint8_t *)"abcd", 4, 1);
    CHECK(!cache_get_queuestatus(&qbuf, &qlen),
          "false when no queueStatus frame added");

    /* Add a queueStatus frame among others. */
    uint8_t qs[7];
    fill_pattern(qs, sizeof(qs), 0xA0);
    CHECK(cache_add_frame(qs, sizeof(qs), QUEUESTATUS_TAG) == 0,
          "add queueStatus frame");
    (void)cache_add_frame((const uint8_t *)"xyz", 3, 2);

    CHECK(cache_get_queuestatus(&qbuf, &qlen),
          "true after queueStatus frame added");
    CHECK(qlen == sizeof(qs), "queueStatus len matches");
    CHECK(check_pattern(qbuf, qlen, 0xA0), "queueStatus bytes round-trip");
}

/* 4a. Byte-cap overflow: reject with -ENOMEM, prior frames intact. */
static void test_overflow_bytes(void)
{
    printf("test_overflow_bytes:\n");
    cache_begin();

    /* Fill the arena nearly to the cap with known frames. Use 256 B chunks
     * (< the 512 B transport cap) and a distinct seed per frame so we can
     * verify none were corrupted by the rejected frame. */
    const uint16_t chunk = 256;
    uint8_t buf[256];
    int added = 0;
    int rc = 0;
    for (;;) {
        uint8_t seed = (uint8_t)(added * 7 + 1);
        fill_pattern(buf, chunk, seed);
        rc = cache_add_frame(buf, chunk, 1);
        if (rc != 0) {
            break;
        }
        added++;
        if (added > CONFIG_CACHE_MAX_FRAMES + 5) {
            break; /* safety: should have hit a cap well before here */
        }
    }

    CHECK(rc == -ENOMEM, "add_frame returns -ENOMEM at the cap");
    CHECK(added > 0, "some frames were accepted before the cap");

    /* Either the byte cap or the frame cap stopped us; assert we are at one. */
    bool at_byte_cap  = ((uint32_t)s_used + chunk > CONFIG_CACHE_ARENA_BYTES);
    bool at_frame_cap = (s_count >= CONFIG_CACHE_MAX_FRAMES);
    CHECK(at_byte_cap || at_frame_cap, "stopped at byte cap or frame cap");

    /* The rejected frame must NOT have advanced the cursor/count. */
    CHECK(cache_frame_count() == (uint16_t)added,
          "count unchanged by rejected frame");

    /* Every previously-stored frame is still byte-for-byte intact. */
    bool all_intact = true;
    for (int i = 0; i < added; i++) {
        const struct cache_frame_ref *r = cache_frame_at((uint16_t)i);
        uint8_t seed = (uint8_t)(i * 7 + 1);
        if (!r || r->len != chunk ||
            !check_pattern(&s_arena[r->offset], r->len, seed)) {
            all_intact = false;
            break;
        }
    }
    CHECK(all_intact, "all prior frames intact after rejection (no corruption)");
}

/* 4b. Frame-cap overflow: filling the index to CONFIG_CACHE_MAX_FRAMES with
 * tiny frames must reject the (MAX+1)th with -ENOMEM, frames intact. */
static void test_overflow_frames(void)
{
    printf("test_overflow_frames:\n");
    cache_begin();

    /* 1-byte frames keep us well under the byte cap so the FRAME cap trips
     * first (128 * 1 B = 128 B << 16 KB). */
    uint8_t one[1];
    int rc = 0;
    for (int i = 0; i < CONFIG_CACHE_MAX_FRAMES; i++) {
        one[0] = (uint8_t)(i & 0xFF);
        rc = cache_add_frame(one, 1, 1);
        if (rc != 0) {
            break;
        }
    }
    CHECK(rc == 0, "all CONFIG_CACHE_MAX_FRAMES tiny frames accepted");
    CHECK(cache_frame_count() == CONFIG_CACHE_MAX_FRAMES,
          "frame count == CONFIG_CACHE_MAX_FRAMES");

    /* One more frame must be rejected by the frame cap. */
    one[0] = 0xEE;
    int over = cache_add_frame(one, 1, 1);
    CHECK(over == -ENOMEM, "frame past the cap returns -ENOMEM");
    CHECK(cache_frame_count() == CONFIG_CACHE_MAX_FRAMES,
          "count unchanged after frame-cap rejection");

    /* Prior frames intact: byte i holds value (i & 0xFF). */
    bool all_intact = true;
    for (int i = 0; i < CONFIG_CACHE_MAX_FRAMES; i++) {
        const struct cache_frame_ref *r = cache_frame_at((uint16_t)i);
        if (!r || r->len != 1 || s_arena[r->offset] != (uint8_t)(i & 0xFF)) {
            all_intact = false;
            break;
        }
    }
    CHECK(all_intact, "all frames intact after frame-cap rejection");
}

/* 5. cache_mark_ready flips cache_is_ready() to true. */
static void test_mark_ready(void)
{
    printf("test_mark_ready:\n");
    cache_begin();
    CHECK(!cache_is_ready(), "not ready before mark_ready");
    cache_mark_ready();
    CHECK(cache_is_ready(), "ready after mark_ready");
}

int main(void)
{
    printf("=== config_cache host tests ===\n");

    test_begin_resets();
    test_pack_and_roundtrip();
    test_get_queuestatus();
    test_overflow_bytes();
    test_overflow_frames();
    test_mark_ready();

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
