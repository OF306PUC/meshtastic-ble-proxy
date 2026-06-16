#ifndef UART_MESHTASTIC_H
#define UART_MESHTASTIC_H

#include <stdint.h>

/*
 * Meshtastic serial framing:
 *   [0x94] [0xC3] [len_hi] [len_lo] [protobuf_payload...]
 *
 * UART1 — P1.01 RX / P1.02 TX — 115200 8N1
 */

#define MESHTASTIC_MAX_PAYLOAD 512

/**
 * Called (in work-queue context) when a complete FromRadio frame is received.
 *
 * @param payload  Protobuf bytes (no header).
 * @param len      Payload length in bytes.
 */
typedef void (*fromradio_uart_cb_t)(const uint8_t *payload, uint16_t len);

/**
 * Initialize UART1 and start receiving Meshtastic frames.
 * Must be called after bt_enable() (so the system workqueue is up).
 */
int uart_meshtastic_init(fromradio_uart_cb_t cb);

/**
 * Send a ToRadio protobuf over UART with Meshtastic framing.
 * Non-blocking: returns immediately; completion signalled via UART_TX_DONE.
 *
 * Packets are queued (FIFO, depth TX_QUEUE_DEPTH=4) and sent one at a time.
 * Safe to call from multiple threads/connections concurrently.
 *
 * @return  0        success (enqueued)
 *         -EMSGSIZE payload > MESHTASTIC_MAX_PAYLOAD
 *         -ENOMEM   TX queue full (all 4 slots occupied)
 */
int uart_meshtastic_tx(const uint8_t *payload, uint16_t len);

#endif /* UART_MESHTASTIC_H */
