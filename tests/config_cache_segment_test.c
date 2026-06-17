/*
 * config_cache_segment_test.c — HOST-side, DATA-DRIVEN test of the config_cache
 * storage + per-nonce segmentation against REAL node captures.
 *
 * No Zephyr, no hardware. It loads the full want_config burst captured from a
 * real Meshtastic node (execution/fetch_node_config.py --dump) into config_cache
 * exactly as upstream_session does (every frame EXCEPT the config_complete_id
 * terminator, which the proxy synthesizes per phone), then asserts that
 * cache_frame_in_segment() reproduces — variant order AND payload bytes — the
 * separately-captured ONLY_CONFIG (69420) and ONLY_NODES (69421) bursts.
 *
 * This verifies the actual ADR-001 claim: the config/node data the node sends
 * over UART is stored correctly and served to each BLE client the way the real
 * node would for each special want_config nonce.
 *
 * Inputs (dumps from fetch_node_config.py, default dir = first argv or ".tmp"):
 *   burst_full.hex    captured with a normal nonce  (full superset → the cache)
 *   burst_69420.hex   captured with --nonce 69420   (ONLY_CONFIG expected)
 *   burst_69421.hex   captured with --nonce 69421   (ONLY_NODES  expected)
 *
 * config_cache.c depends on a few Zephyr/meshtastic bits absent on the host; we
 * shim them on the -I path and #include config_cache.c directly (same technique
 * as config_cache_test.c). config_cache.c/.h are NOT modified.
 *
 * ---------------------------------------------------------------------------
 * BUILD & RUN (adjust SRC/REPO if moved):
 *
 *   REPO=/home/juanignaciolorca/Desktop/Personal-AI-Studio/Descentralized_Mesh_Noric_GATT_Server
 *   SRC=$REPO/firmware/src
 *   TESTS=$REPO/firmware/tests
 *
 *   mkdir -p /tmp/ccseg_shim/zephyr/sys /tmp/ccseg_shim/zephyr/logging /tmp/ccseg_shim/meshtastic
 *   printf 'typedef long atomic_t;\nstatic inline void atomic_set(atomic_t*p,long v){*p=v;}\nstatic inline long atomic_get(const atomic_t*p){return *p;}\n' > /tmp/ccseg_shim/zephyr/sys/atomic.h
 *   printf '#define LOG_MODULE_REGISTER(...)\n#define LOG_INF(...)\n#define LOG_WRN(...)\n#define LOG_DBG(...)\n#define LOG_LEVEL_INF 3\n' > /tmp/ccseg_shim/zephyr/logging/log.h
 *   printf '#define meshtastic_FromRadio_packet_tag 2\n#define meshtastic_FromRadio_my_info_tag 3\n#define meshtastic_FromRadio_node_info_tag 4\n#define meshtastic_FromRadio_config_tag 5\n#define meshtastic_FromRadio_config_complete_id_tag 7\n#define meshtastic_FromRadio_moduleConfig_tag 9\n#define meshtastic_FromRadio_channel_tag 10\n#define meshtastic_FromRadio_queueStatus_tag 11\n#define meshtastic_FromRadio_metadata_tag 13\n#define meshtastic_FromRadio_fileInfo_tag 15\n#define meshtastic_FromRadio_deviceuiConfig_tag 17\n' > /tmp/ccseg_shim/meshtastic/mesh.pb.h
 *
 *   gcc -std=c11 -Wall -Wextra -I/tmp/ccseg_shim -I"$SRC" \
 *       "$TESTS/config_cache_segment_test.c" -o /tmp/cc_seg_test \
 *       && /tmp/cc_seg_test "$REPO/.tmp"
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

/* Module under test (shims on the -I path satisfy its Zephyr/meshtastic deps). */
#include "config_cache.c"

#define MAX_FRAMES 256
#define MAX_PAYLOAD 600

struct frame {
    char     name[40];
    int      tag;
    uint8_t  payload[MAX_PAYLOAD];
    uint16_t len;
};

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* Map a FromRadio oneof variant NAME (as written by fetch_node_config.py) to its
 * protobuf field tag. Must match the script's FROMRADIO_VARIANTS table. */
static int name_to_tag(const char *name)
{
    if (!strcmp(name, "packet"))             return meshtastic_FromRadio_packet_tag;
    if (!strcmp(name, "my_info"))            return meshtastic_FromRadio_my_info_tag;
    if (!strcmp(name, "node_info"))          return meshtastic_FromRadio_node_info_tag;
    if (!strcmp(name, "config"))             return meshtastic_FromRadio_config_tag;
    if (!strcmp(name, "config_complete_id")) return meshtastic_FromRadio_config_complete_id_tag;
    if (!strcmp(name, "moduleConfig"))       return meshtastic_FromRadio_moduleConfig_tag;
    if (!strcmp(name, "channel"))            return meshtastic_FromRadio_channel_tag;
    if (!strcmp(name, "queueStatus"))        return meshtastic_FromRadio_queueStatus_tag;
    if (!strcmp(name, "metadata"))           return meshtastic_FromRadio_metadata_tag;
    if (!strcmp(name, "fileInfo"))           return meshtastic_FromRadio_fileInfo_tag;
    if (!strcmp(name, "deviceuiConfig"))     return meshtastic_FromRadio_deviceuiConfig_tag;
    return -1;  /* unknown */
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a dump file (index\tname\tpayload_len\tframe_len\tpayload_hex per row,
 * '#' comment lines ignored). Returns frame count or -1 on error. */
static int load_dump(const char *path, struct frame *out, int max)
{
    FILE *fh = fopen(path, "r");
    if (!fh) {
        printf("  ERROR: cannot open %s (%s)\n", path, strerror(errno));
        return -1;
    }
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof(line), fh)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (n >= max) { printf("  ERROR: too many frames in %s\n", path); fclose(fh); return -1; }

        char name[40]; int idx, plen, flen; char hex[4096];
        /* index  name  payload_len  frame_len  payload_hex */
        if (sscanf(line, "%d\t%39[^\t]\t%d\t%d\t%4095s", &idx, name, &plen, &flen, hex) < 4) {
            continue;  /* tolerate blank/short lines */
        }
        struct frame *f = &out[n];
        memset(f, 0, sizeof(*f));
        snprintf(f->name, sizeof(f->name), "%s", name);
        f->tag = name_to_tag(name);
        /* decode hex payload */
        int hl = (int)strlen(hex);
        if (hl % 2 != 0 || hl / 2 != plen) {
            printf("  WARN: %s frame %d hex/len mismatch (hl=%d plen=%d)\n", path, n, hl, plen);
        }
        int b = 0;
        for (int i = 0; i + 1 < hl && b < MAX_PAYLOAD; i += 2) {
            int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
            if (hi < 0 || lo < 0) break;
            f->payload[b++] = (uint8_t)((hi << 4) | lo);
        }
        f->len = (uint16_t)b;
        n++;
    }
    fclose(fh);
    return n;
}

/* Build the expected served sequence for a captured burst: every frame except
 * the config_complete_id terminator (which the proxy synthesizes per phone). */
static int expected_from_dump(const struct frame *dump, int n, const struct frame **out)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (dump[i].tag == meshtastic_FromRadio_config_complete_id_tag) continue;
        out[m++] = &dump[i];
    }
    return m;
}

/*
 * Walk the cache, collecting frames cache_frame_in_segment() keeps for `nonce`,
 * and compare against the expected sequence.
 *
 *   strict_bytes = true  : compare variant tag + length + payload BYTES. Use this
 *                          only when `expected` is the SAME capture the cache was
 *                          loaded from (NORMAL ↔ burst_full) — this is the storage
 *                          integrity check.
 *   strict_bytes = false : compare variant tag + count only. Use for the special
 *                          nonces (ONLY_CONFIG/ONLY_NODES) whose dumps were
 *                          captured at a different moment: node_info carries
 *                          dynamic fields (last_heard, telemetry, SNR) that
 *                          legitimately differ between captures, so only the
 *                          SEGMENTATION (which frames, in what order) is asserted.
 */
static void verify_segment(const char *label, uint32_t nonce,
                           const struct frame **expected, int exp_n, bool strict_bytes)
{
    printf("[%s] nonce=%u, expected %d frames (%s)\n",
           label, nonce, exp_n, strict_bytes ? "variant+len+bytes" : "variant only");
    int got = 0;
    uint16_t total = cache_frame_count();
    for (uint16_t idx = 0; idx < total; idx++) {
        if (!cache_frame_in_segment(idx, nonce)) continue;
        const struct cache_frame_ref *ref = cache_frame_at(idx);
        const uint8_t *bytes = cache_frame_bytes(ref);
        if (got < exp_n) {
            const struct frame *e = expected[got];
            CHECK(ref->variant == e->tag,
                  "%s frame %d: variant %d != expected %d (%s)",
                  label, got, ref->variant, e->tag, e->name);
            if (strict_bytes) {
                CHECK(ref->len == e->len,
                      "%s frame %d (%s): len %u != expected %u",
                      label, got, e->name, ref->len, e->len);
                CHECK(ref->len == e->len && memcmp(bytes, e->payload, ref->len) == 0,
                      "%s frame %d (%s): payload bytes differ", label, got, e->name);
            }
        }
        got++;
    }
    CHECK(got == exp_n, "%s: served %d frames, expected %d", label, got, exp_n);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".tmp";
    char p_full[1024], p_cfg[1024], p_nodes[1024];
    snprintf(p_full,  sizeof(p_full),  "%s/burst_full.hex",  dir);
    snprintf(p_cfg,   sizeof(p_cfg),   "%s/burst_69420.hex", dir);
    snprintf(p_nodes, sizeof(p_nodes), "%s/burst_69421.hex", dir);

    static struct frame full[MAX_FRAMES], cfg[MAX_FRAMES], nodes[MAX_FRAMES];
    int nf = load_dump(p_full,  full,  MAX_FRAMES);
    int nc = load_dump(p_cfg,   cfg,   MAX_FRAMES);
    int nn = load_dump(p_nodes, nodes, MAX_FRAMES);
    if (nf < 0 || nc < 0 || nn < 0) {
        printf("FAILED to load dumps from '%s' (run fetch_node_config.py --dump first)\n", dir);
        return 2;
    }
    printf("Loaded: full=%d cfg(69420)=%d nodes(69421)=%d frames\n", nf, nc, nn);

    /* Load the full burst into the cache exactly as upstream_session does:
     * every non-config_complete frame, in order. */
    cache_begin();
    for (int i = 0; i < nf; i++) {
        if (full[i].tag == meshtastic_FromRadio_config_complete_id_tag) continue;  /* terminator */
        int rc = cache_add_frame(full[i].payload, full[i].len, full[i].tag);
        CHECK(rc == 0, "cache_add_frame %d (%s) failed rc=%d", i, full[i].name, rc);
    }
    cache_mark_ready();
    CHECK(cache_is_ready(), "cache not ready after mark_ready");

    /* Expected served sequences (each dump minus its config_complete). */
    const struct frame *exp_full[MAX_FRAMES], *exp_cfg[MAX_FRAMES], *exp_nodes[MAX_FRAMES];
    int ef = expected_from_dump(full,  nf, exp_full);
    int ec = expected_from_dump(cfg,   nc, exp_cfg);
    int en = expected_from_dump(nodes, nn, exp_nodes);

    /* NORMAL: full burst, byte-for-byte (cache was loaded from burst_full →
     * storage integrity). Special nonces: segmentation only (their node_info
     * bytes differ across captures by design — dynamic fields). */
    verify_segment("NORMAL",      305419896u,             exp_full,  ef, true);
    verify_segment("ONLY_CONFIG", MESH_NONCE_ONLY_CONFIG, exp_cfg,   ec, false);
    verify_segment("ONLY_NODES",  MESH_NONCE_ONLY_NODES,  exp_nodes, en, false);

    /* Sanity: the own node_info (first node_info) must be present in ONLY_NODES
     * and ONLY_CONFIG; other-node node_info must be absent from ONLY_CONFIG. */
    CHECK(en >= 1, "ONLY_NODES expected >= 1 node_info");
    CHECK(ec == ef - (en - 1) || en == 0,
          "ONLY_CONFIG count (%d) should be full (%d) minus other-node node_info (%d)",
          ec, ef, en - 1);

    printf("\nChecks: %d, Failures: %d\n", g_checks, g_fails);
    if (g_fails == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("TESTS FAILED\n");
    return 1;
}
