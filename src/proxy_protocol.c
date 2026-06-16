/*
 * proxy_protocol.c — application-layer proxy header parser / builder
 *
 * No Zephyr or Meshtastic dependencies — pure C for portability and testability.
 */

#include "proxy_protocol.h"
#include <string.h>

bool proxy_header_parse(const uint8_t *payload, uint16_t len,
                        struct proxy_header *out)
{
    if (len < PROXY_HEADER_SIZE) {
        return false;
    }
    if (payload[PROXY_OFF_VERSION] != PROXY_VERSION) {
        return false;
    }

    memcpy(out->src.bytes, &payload[PROXY_OFF_SRC], PROXY_ID_SIZE);
    memcpy(out->dst.bytes, &payload[PROXY_OFF_DST], PROXY_ID_SIZE);
    out->content     = &payload[PROXY_OFF_CONTENT];
    out->content_len = len - PROXY_HEADER_SIZE;

    return true;
}

uint16_t proxy_header_build(const proxy_id_t *src,
                            const proxy_id_t *dst,
                            const uint8_t    *content,
                            uint16_t          content_len,
                            uint8_t          *dst_buf,
                            uint16_t          dst_buf_size)
{
    if (content_len > PROXY_CONTENT_MAX) {
        return 0U;
    }

    uint16_t total = (uint16_t)(PROXY_HEADER_SIZE + content_len);
    if (total > dst_buf_size) {
        return 0U;
    }

    dst_buf[PROXY_OFF_VERSION] = PROXY_VERSION;
    memcpy(&dst_buf[PROXY_OFF_SRC], src->bytes, PROXY_ID_SIZE);
    memcpy(&dst_buf[PROXY_OFF_DST], dst->bytes, PROXY_ID_SIZE);

    if (content_len > 0U && content != NULL) {
        memcpy(&dst_buf[PROXY_OFF_CONTENT], content, content_len);
    }

    return total;
}

bool proxy_id_equal(const proxy_id_t *a, const proxy_id_t *b)
{
    return memcmp(a->bytes, b->bytes, PROXY_ID_SIZE) == 0;
}

bool proxy_id_is_zero(const proxy_id_t *id)
{
    for (uint8_t i = 0U; i < PROXY_ID_SIZE; i++) {
        if (id->bytes[i] != 0U) {
            return false;
        }
    }
    return true;
}

void proxy_id_from_str(proxy_id_t *id, const char *str)
{
    memset(id->bytes, 0, PROXY_ID_SIZE);
    if (str == NULL) {
        return;
    }
    size_t len = strlen(str);
    if (len > PROXY_ID_SIZE) {
        len = PROXY_ID_SIZE;
    }
    memcpy(id->bytes, str, len);
}
