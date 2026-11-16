/**
 * @file ble_gatt_client.h
 * @brief Active Bluetooth LE GATT Client using Linux L2CAP ATT Sockets
 *
 * Implements active connections and GATT transactions for ESPHome Bluetooth Proxy.
 */

#ifndef BLE_GATT_CLIENT_H
#define BLE_GATT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_MAX_CONNECTIONS 3

typedef struct {
    void (*on_device_connection)(uint64_t address, bool connected, uint32_t mtu, int32_t error, void *user_data);
    void (*on_connections_free)(uint32_t free, uint32_t limit, const uint64_t *allocated, size_t count, void *user_data);
    void (*on_gatt_services_done)(uint64_t address, void *user_data);
    void (*on_gatt_read_response)(uint64_t address, uint16_t handle, const uint8_t *data, size_t len, void *user_data);
    void (*on_gatt_write_response)(uint64_t address, uint16_t handle, void *user_data);
    void (*on_gatt_notify_data)(uint64_t address, uint16_t handle, const uint8_t *data, size_t len, void *user_data);
} ble_gatt_callbacks_t;

int ble_gatt_client_init(const ble_gatt_callbacks_t *cbs, void *user_data);
void ble_gatt_client_cleanup(void);

uint32_t ble_gatt_get_free_connections(uint64_t *allocated_out, uint32_t max_count);

int ble_gatt_connect(uint64_t address, uint8_t address_type);
int ble_gatt_disconnect(uint64_t address);

int ble_gatt_get_services(uint64_t address);
int ble_gatt_read(uint64_t address, uint16_t handle);
int ble_gatt_write(uint64_t address, uint16_t handle, const uint8_t *data, size_t len, bool response);
int ble_gatt_notify(uint64_t address, uint16_t handle, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* BLE_GATT_CLIENT_H */
