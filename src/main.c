#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>
#include "ble_gatt.h"
#include "uart_meshtastic.h"
#include "proto_handler.h"
#include "router.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/*
 * Called (system work-queue context) when the UART driver has assembled a
 * complete FromRadio frame from the Meshtastic node.
 *
 * Flow:
 *   1. Decode the protobuf (nanopb) to extract routing fields.
 *   2. Dispatch raw bytes to the correct BLE connection(s) via the router.
 *
 * On decode failure the raw bytes are still broadcast so no packet is dropped.
 */
static void on_fromradio_uart(const uint8_t *payload, uint16_t len)
{
    struct fromradio_info info;

    if (proto_decode_fromradio(payload, len, &info) != 0) {
        /* Decode failed (malformed frame?). Broadcast raw as fallback. */
        LOG_WRN("FromRadio decode failed — broadcasting raw (%d B)", len);
        ble_gatt_broadcast_fromradio(payload, len);
        return;
    }

    router_dispatch(payload, len, &info);
}

/*
 * Called when a phone writes a ToRadio protobuf to the GATT TORADIO
 * characteristic. The bytes are already protobuf-encoded by the phone's app;
 * we add the Meshtastic UART framing and push to the TX queue.
 */
static void on_toradio_ble(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    LOG_INF("ToRadio: %d bytes from conn %p → UART", len, (void *)conn);

    int err = uart_meshtastic_tx(data, len);
    if (err == -ENOMEM) {
        LOG_WRN("TX queue full — ToRadio from conn %p dropped", (void *)conn);
    } else if (err) {
        LOG_ERR("uart_meshtastic_tx: %d", err);
    }
}

int main(void)
{
    int err;

    LOG_INF("=== Meshtastic BLE Proxy starting ===");

    /* 1. Init GATT service (must happen before bt_enable). */
    err = ble_gatt_init(on_toradio_ble);
    if (err) {
        LOG_ERR("ble_gatt_init: %d", err);
        return err;
    }

    /* 2. Enable Bluetooth stack. */
    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("bt_enable: %d", err);
        return err;
    }
    LOG_INF("Bluetooth ready");

    /* 3. Start advertising as a Meshtastic-compatible peripheral. */
    err = ble_gatt_start_advertising();
    if (err) {
        return err;
    }

    /* 4. Init UART driver — starts DMA reception immediately.
     *    The Meshtastic node will begin sending FromRadio frames
     *    once it receives a want_config_id ToRadio (sent by the phone). */
    err = uart_meshtastic_init(on_fromradio_uart);
    if (err) {
        LOG_ERR("uart_meshtastic_init: %d", err);
        return err;
    }

    /* Main loop — all work runs in the system work queue and BT callbacks. */
    while (1) {
        k_sleep(K_MSEC(100));
    }

    return 0;
}
