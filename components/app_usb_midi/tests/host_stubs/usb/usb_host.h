// SPDX-License-Identifier: Apache-2.0
/* Minimal test declarations match the official espressif/usb 1.6 public API.
 * These are fake backends, not a replacement driver or firmware include path. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef struct fake_client *usb_host_client_handle_t;
typedef struct fake_device *usb_device_handle_t;
typedef enum {
    USB_TRANSFER_STATUS_COMPLETED,
    USB_TRANSFER_STATUS_ERROR,
    USB_TRANSFER_STATUS_CANCELLED,
} usb_transfer_status_t;
typedef struct usb_transfer_s usb_transfer_t;
struct usb_transfer_s {
    uint8_t *data_buffer;
    size_t data_buffer_size;
    int num_bytes;
    int actual_num_bytes;
    uint32_t flags;
    usb_device_handle_t device_handle;
    uint8_t bEndpointAddress;
    usb_transfer_status_t status;
    uint32_t timeout_ms;
    void (*callback)(usb_transfer_t *);
    void *context;
    int num_isoc_packets;
};
typedef enum {
    USB_HOST_CLIENT_EVENT_NEW_DEV,
    USB_HOST_CLIENT_EVENT_DEV_GONE,
} usb_host_client_event_t;
typedef struct {
    usb_host_client_event_t event;
    union {
        struct {
            uint8_t address;
        } new_dev;
        struct {
            usb_device_handle_t dev_hdl;
        } dev_gone;
    };
} usb_host_client_event_msg_t;
typedef struct {
    bool is_synchronous;
    int max_num_event_msg;
    struct {
        void (*client_event_callback)(const usb_host_client_event_msg_t *, void *);
        void *callback_arg;
    } async;
} usb_host_client_config_t;
typedef struct {
    bool skip_phy_setup;
    bool root_port_unpowered;
    int intr_flags;
    unsigned peripheral_map;
} usb_host_config_t;
typedef struct {
    uint8_t bytes[9];
} usb_config_desc_t;
typedef struct {
    uint16_t idVendor;
    uint16_t idProduct;
} usb_device_desc_t;
#define USB_HOST_LIB_EVENT_FLAGS_ALL_FREE 2U
esp_err_t usb_host_install(const usb_host_config_t *config);
esp_err_t usb_host_uninstall(void);
esp_err_t usb_host_lib_handle_events(TickType_t wait, uint32_t *events);
esp_err_t usb_host_device_free_all(void);
esp_err_t usb_host_client_register(const usb_host_client_config_t *, usb_host_client_handle_t *);
esp_err_t usb_host_client_deregister(usb_host_client_handle_t client);
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t client, TickType_t wait);
esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t *);
esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t);
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t, const usb_config_desc_t **);
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t, const usb_device_desc_t **);
esp_err_t usb_host_interface_claim(usb_host_client_handle_t, usb_device_handle_t, uint8_t, uint8_t);
esp_err_t usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, uint8_t);
esp_err_t usb_host_endpoint_halt(usb_device_handle_t, uint8_t);
esp_err_t usb_host_endpoint_flush(usb_device_handle_t, uint8_t);
esp_err_t usb_host_transfer_alloc(size_t, int, usb_transfer_t **);
esp_err_t usb_host_transfer_free(usb_transfer_t *);
esp_err_t usb_host_transfer_submit(usb_transfer_t *);
