#ifndef BLE_GATT_H
#define BLE_GATT_H

#include <stdint.h>
#include <zephyr/bluetooth/conn.h>
#include "proxy_protocol.h"

/* Max simultaneous BLE connections (phones).
 * Tied to CONFIG_BT_MAX_CONN so the app-layer slot array always matches the
 * BT stack limit. Change prj.conf (CONFIG_BT_MAX_CONN) to resize both at once. */
#define MAX_BLE_CONNECTIONS     CONFIG_BT_MAX_CONN

/* Per-connection FromRadio packet queue depth */
#define FROMRADIO_QUEUE_DEPTH   8

/* Max size of a single FromRadio protobuf packet (bytes) */
#define FROMRADIO_MAX_PKT_SIZE  512

/**
 * Callback fired when a connected phone writes to the TORADIO characteristic.
 *
 * @param conn  BLE connection that sent the data.
 * @param data  Raw protobuf bytes (ToRadio message, already encoded by the app).
 * @param len   Length in bytes.
 */
typedef void (*toradio_cb_t)(struct bt_conn *conn, const uint8_t *data, uint16_t len);

/** Initialize GATT service and connection tracking. Must be called before bt_enable(). */
int ble_gatt_init(toradio_cb_t cb);

/** Start BLE advertising (Meshtastic-compatible). Called at boot and after each connection. */
int ble_gatt_start_advertising(void);

/**
 * Enqueue a FromRadio packet for a specific BLE connection.
 * Triggers a FROMNUM notification to the phone.
 *
 * @return  0        success
 *         -ENOENT   connection not tracked
 *         -ENOMEM   per-connection queue full
 *         -EMSGSIZE packet exceeds FROMRADIO_MAX_PKT_SIZE
 */
int ble_gatt_enqueue_fromradio(struct bt_conn *conn, const uint8_t *data, uint16_t len);

/**
 * Enqueue a FromRadio packet for ALL currently connected BLE clients.
 * Used for config/meta variants and broadcast mesh packets.
 *
 * @return Number of connections that received the packet (0 if none connected).
 */
int ble_gatt_broadcast_fromradio(const uint8_t *data, uint16_t len);

/**
 * Return the BLE connection whose registered proxy_id matches the given id.
 * Returns NULL if no connection has registered that id.
 *
 * Used by the router to target a specific phone by its proxy identifier.
 */
struct bt_conn *ble_gatt_get_conn_by_proxy_id(const proxy_id_t *id);

/**
 * Register a proxy identifier (phone number, UUID, etc.) for a BLE connection.
 *
 * Called from the NODE_REG GATT write handler when a phone writes its 16-byte
 * identifier.  The router uses this mapping to perform targeted delivery.
 *
 * @param conn  The BLE connection registering.
 * @param id    16-byte proxy identifier written by the phone.
 * @return 0 on success, -ENOENT if connection is not tracked.
 */
int ble_gatt_register_proxy_id(struct bt_conn *conn, const proxy_id_t *id);

#endif /* BLE_GATT_H */
