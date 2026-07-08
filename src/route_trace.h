/*
 * route_trace.h — compile-time-gated ROUTE message-path tracing.
 *
 * The ROUTE_* macros expand to Zephyr LOG_INF / LOG_HEXDUMP_INF only when
 * CONFIG_MESHTASTIC_ROUTE_TRACE is set; otherwise they compile to nothing, so
 * the per-packet tracing adds zero processing time on the hot path (arguments
 * are not evaluated when the trace is off). Payload hexdumps are further gated
 * behind CONFIG_MESHTASTIC_ROUTE_TRACE_PAYLOAD.
 *
 * Each translation unit that uses these must have registered its own log module
 * (LOG_MODULE_REGISTER) — the macros emit under that module, exactly like a
 * direct LOG_INF call would.
 */
#ifndef ROUTE_TRACE_H
#define ROUTE_TRACE_H

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>   /* IS_ENABLED */

#if IS_ENABLED(CONFIG_MESHTASTIC_ROUTE_TRACE)
#define ROUTE_TRACE(...)  LOG_INF(__VA_ARGS__)
#else
#define ROUTE_TRACE(...)  ((void)0)
#endif

#if IS_ENABLED(CONFIG_MESHTASTIC_ROUTE_TRACE) && \
    IS_ENABLED(CONFIG_MESHTASTIC_ROUTE_TRACE_PAYLOAD)
#define ROUTE_TRACE_HEXDUMP(_data, _len, _label)  LOG_HEXDUMP_INF(_data, _len, _label)
#else
#define ROUTE_TRACE_HEXDUMP(_data, _len, _label)  ((void)0)
#endif

#endif /* ROUTE_TRACE_H */
