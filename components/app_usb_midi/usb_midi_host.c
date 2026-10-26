// SPDX-License-Identifier: Apache-2.0
/* One product task owns the controller role and every Host/client/transfer
 * lifetime. USB callbacks only publish completion flags into that task. */
#include "app_usb_midi.h"
#include "app_usb_midi_descriptor.h"
#include "app_usb_midi_packets.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_iris.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/usb_dwc_ll.h"
#include "hal/usb_utmi_ll.h"
#include "soc/interrupts.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#define USB_OWNER_STACK_BYTES 6144U
#define USB_OWNER_PRIORITY 4U
#define USB_TRANSITION_MS 2000U
#define USB_EXTERNAL_PANIC_MS 100U
#define USB_EVENT_WAIT_MS 10U
#define USB_TX_CAPACITY 64U
#define USB_CLIENT_EVENT_CAPACITY 8U

/* One physical S31 controller, hence one product owner. These atomic flags are
 * the only shutdown-shared state; the callback never dereferences an instance. */
static atomic_bool s_owner_claimed;
static atomic_bool s_host_hardware_active;
static atomic_bool s_shutdown_requested;

static void host_emergency_shutdown(void)
{
    atomic_store_explicit(&s_shutdown_requested, true, memory_order_release);
    if (atomic_exchange_explicit(&s_host_hardware_active, false, memory_order_acq_rel)) {
        /* The pinned S31 CPU reset does not reset HS DMA. Do not wait, allocate
         * or free here: a normal role transition uses complete Host teardown. */
        for (int core = 0; core < SOC_CPU_CORES_NUM; ++core) {
            esp_rom_route_intr_matrix(core, ETS_USB_OTGHS_INTR_SOURCE, ETS_INVALID_INUM);
        }
        if (_usb_utmi_ll_bus_clock_is_enabled()) {
            usb_dwc_ll_gahbcfg_dis_global_intr(USB_DWC_LL_GET_HW(0));
            usb_utmi_ll_reset_register();
            /* esp_restart invokes hooks before pausing the other core. An
             * already pending/entered HCD ISR may still read its MMIO, so
             * leave the reset controller clocked until the CPU reset. */
        }
    }
}

typedef struct {
    uint8_t bytes[4];
} midi_packet_t;

struct app_usb_midi {
    app_note_router_t *router;
    SemaphoreHandle_t status_lock;
    SemaphoreHandle_t finished;
    QueueHandle_t transmit_queue;
    TaskHandle_t task;
    atomic_int requested_mode;
    atomic_uint request_sequence;
    atomic_bool stopping;
    atomic_bool transmit_ready;
    atomic_bool transmit_panic;
    app_usb_midi_status_t status;
    bool library_installed;
    usb_host_client_handle_t client;
    usb_device_handle_t device;
    app_usb_midi_interface_t interface;
    uint8_t candidate_address;
    bool claimed;
    bool closing;
    bool gone;
    bool rx_inflight;
    bool tx_inflight;
    bool rx_completed;
    bool tx_completed;
    bool vendor_recovery_pending;
    uint64_t vendor_retry_ms;
    esp_err_t vendor_recovery_error;
    usb_transfer_t *receive;
    usb_transfer_t *transmit;
};

static uint64_t milliseconds(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static void status_lock(app_usb_midi_t *usb)
{
    (void)xSemaphoreTake(usb->status_lock, portMAX_DELAY);
}

static void status_unlock(app_usb_midi_t *usb)
{
    (void)xSemaphoreGive(usb->status_lock);
}

static void set_state(app_usb_midi_t *usb, app_usb_mode_t actual,
                       app_usb_state_t state, esp_err_t error)
{
    status_lock(usb);
    usb->status.actual_mode = actual;
    usb->status.state = state;
    usb->status.last_error = error;
    status_unlock(usb);
}

static void received_count(app_usb_midi_t *usb, unsigned valid, unsigned invalid)
{
    status_lock(usb);
    usb->status.received_packets += valid;
    usb->status.dropped_packets += invalid;
    status_unlock(usb);
}

static bool queue_transmit(void *context, const uint8_t bytes[4])
{
    app_usb_midi_t *usb = context;
    if (!atomic_load_explicit(&usb->transmit_ready, memory_order_acquire) ||
            atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        return true; /* Vendor/no OUT endpoint: no external note was started. */
    }
    midi_packet_t packet;
    memcpy(packet.bytes, bytes, sizeof(packet.bytes));
    if (xQueueSend(usb->transmit_queue, &packet, 0) == pdTRUE) {
        return true;
    }
    /* A rejected Note On can make the caller withdraw its gesture, so any
     * overflow schedules external All Sound Off after earlier in-flight TX. */
    atomic_store_explicit(&usb->transmit_panic, true, memory_order_release);
    return false;
}

static void client_event(const usb_host_client_event_msg_t *event, void *context)
{
    app_usb_midi_t *usb = context;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV && usb->device == NULL &&
            usb->candidate_address == 0 && !usb->closing) {
        usb->candidate_address = event->new_dev.address;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
               event->dev_gone.dev_hdl == usb->device) {
        usb->gone = true;
    }
}

static void receive_complete(usb_transfer_t *transfer)
{
    app_usb_midi_t *usb = transfer->context;
    usb->rx_inflight = false;
    usb->rx_completed = true;
}

static void transmit_complete(usb_transfer_t *transfer)
{
    app_usb_midi_t *usb = transfer->context;
    usb->tx_inflight = false;
    usb->tx_completed = true;
}

static void pump_events(app_usb_midi_t *usb, TickType_t wait)
{
    if (atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        return;
    }
    if (usb->library_installed) {
        uint32_t events = 0;
        (void)usb_host_lib_handle_events(0, &events);
    }
    if (usb->client != NULL &&
            !atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        (void)usb_host_client_handle_events(usb->client, wait);
    } else if (wait) {
        vTaskDelay(wait);
    }
}

static void clear_device_status(app_usb_midi_t *usb)
{
    status_lock(usb);
    usb->status.connected = false;
    usb->status.midi_in = false;
    usb->status.midi_out = false;
    usb->status.vendor_id = 0;
    usb->status.product_id = 0;
    status_unlock(usb);
}

static esp_err_t submit_transmit(app_usb_midi_t *usb, unsigned bytes)
{
    if (atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    usb->transmit->num_bytes = bytes;
    usb->transmit->device_handle = usb->device;
    usb->transmit->bEndpointAddress = usb->interface.endpoint_out;
    usb->transmit->callback = transmit_complete;
    usb->transmit->context = usb;
    const esp_err_t result = usb_host_transfer_submit(usb->transmit);
    usb->tx_inflight = result == ESP_OK;
    return result;
}

static void send_external_panic(app_usb_midi_t *usb)
{
    if (usb->device == NULL || usb->transmit == NULL || usb->gone) {
        return;
    }
    const uint64_t deadline = milliseconds() + USB_EXTERNAL_PANIC_MS;
    while (usb->tx_inflight && milliseconds() < deadline) {
        pump_events(usb, pdMS_TO_TICKS(USB_EVENT_WAIT_MS));
    }
    if (!usb->tx_inflight && !usb->gone) {
        const uint8_t packet[4] = {0x0b, 0xb0, 120, 0};
        memcpy(usb->transmit->data_buffer, packet, sizeof(packet));
        if (submit_transmit(usb, sizeof(packet)) == ESP_OK) {
            while (usb->tx_inflight && milliseconds() < deadline) {
                pump_events(usb, pdMS_TO_TICKS(USB_EVENT_WAIT_MS));
            }
        }
    }
}

static esp_err_t close_device(app_usb_midi_t *usb, uint64_t deadline)
{
    atomic_store_explicit(&usb->transmit_ready, false, memory_order_release);
    app_note_router_midi_disconnect(usb->router);
    (void)xQueueReset(usb->transmit_queue);
    usb->closing = true;
    if (usb->device != NULL) {
        if (usb->interface.endpoint_in && usb->rx_inflight) {
            (void)usb_host_endpoint_halt(usb->device, usb->interface.endpoint_in);
            (void)usb_host_endpoint_flush(usb->device, usb->interface.endpoint_in);
        }
        if (usb->interface.endpoint_out && usb->tx_inflight) {
            (void)usb_host_endpoint_halt(usb->device, usb->interface.endpoint_out);
            (void)usb_host_endpoint_flush(usb->device, usb->interface.endpoint_out);
        }
        while ((usb->rx_inflight || usb->tx_inflight) && milliseconds() < deadline) {
            pump_events(usb, pdMS_TO_TICKS(USB_EVENT_WAIT_MS));
        }
        if (usb->rx_inflight || usb->tx_inflight) {
            return ESP_ERR_TIMEOUT; /* Keep every in-flight buffer/context alive. */
        }
        if (usb->claimed) {
            const esp_err_t released = usb_host_interface_release(usb->client,
                                          usb->device, usb->interface.interface_number);
            if (released != ESP_OK) {
                return released;
            }
            usb->claimed = false;
        }
        const esp_err_t closed = usb_host_device_close(usb->client, usb->device);
        if (closed != ESP_OK) {
            return closed;
        }
        usb->device = NULL;
    }
    if (usb->receive != NULL) {
        (void)usb_host_transfer_free(usb->receive);
        usb->receive = NULL;
    }
    if (usb->transmit != NULL) {
        (void)usb_host_transfer_free(usb->transmit);
        usb->transmit = NULL;
    }
    usb->rx_completed = false;
    usb->tx_completed = false;
    usb->gone = false;
    usb->closing = false;
    usb->candidate_address = 0;
    memset(&usb->interface, 0, sizeof(usb->interface));
    clear_device_status(usb);
    return ESP_OK;
}

static esp_err_t stop_host(app_usb_midi_t *usb)
{
    if (atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!usb->library_installed) {
        return ESP_OK;
    }
    atomic_store_explicit(&usb->transmit_ready, false, memory_order_release);
    usb->closing = true;
    send_external_panic(usb);
    const uint64_t deadline = milliseconds() + USB_TRANSITION_MS;
    esp_err_t result = close_device(usb, deadline);
    if (result != ESP_OK) {
        return result;
    }
    usb->closing = true; /* Ignore arrivals while deregistering/freeing devices. */
    if (usb->client != NULL) {
        result = usb_host_client_deregister(usb->client);
        if (result != ESP_OK) {
            return result;
        }
        usb->client = NULL;
    }
    result = usb_host_device_free_all();
    if (result != ESP_OK && result != ESP_ERR_NOT_FINISHED) {
        return result;
    }
    bool all_free = result == ESP_OK;
    while (!all_free && milliseconds() < deadline) {
        uint32_t events = 0;
        const esp_err_t handled = usb_host_lib_handle_events(pdMS_TO_TICKS(USB_EVENT_WAIT_MS), &events);
        if (handled != ESP_OK && handled != ESP_ERR_TIMEOUT) {
            return handled;
        }
        all_free = (events & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0;
    }
    if (!all_free) {
        return ESP_ERR_TIMEOUT;
    }
    do {
        uint32_t events = 0;
        (void)usb_host_lib_handle_events(0, &events);
        result = usb_host_uninstall();
        if (result == ESP_OK) {
            usb->library_installed = false;
            usb->closing = false;
            atomic_store_explicit(&s_host_hardware_active, false, memory_order_release);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(USB_EVENT_WAIT_MS));
    } while (result == ESP_ERR_INVALID_STATE && milliseconds() < deadline);
    return result;
}

static esp_err_t start_host(app_usb_midi_t *usb)
{
    if (atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .peripheral_map = 1U, /* S31's only UTMI/OTG controller. */
    };
    atomic_store_explicit(&s_host_hardware_active, true, memory_order_release);
    esp_err_t result = usb_host_install(&host_config);
    if (result != ESP_OK) {
        atomic_store_explicit(&s_host_hardware_active, false, memory_order_release);
        return result;
    }
    usb->library_installed = true;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = USB_CLIENT_EVENT_CAPACITY,
        .async = {.client_event_callback = client_event, .callback_arg = usb},
    };
    result = usb_host_client_register(&client_config, &usb->client);
    if (result != ESP_OK) {
        (void)stop_host(usb);
    }
    return result;
}

static esp_err_t open_candidate(app_usb_midi_t *usb)
{
    if (atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t address = usb->candidate_address;
    usb->candidate_address = 0;
    esp_err_t result = usb_host_device_open(usb->client, address, &usb->device);
    if (result != ESP_OK) {
        return result;
    }
    const usb_config_desc_t *configuration = NULL;
    const usb_device_desc_t *device_descriptor = NULL;
    result = usb_host_get_active_config_descriptor(usb->device, &configuration);
    if (result == ESP_OK) {
        const uint8_t *bytes = (const uint8_t *)configuration;
        const size_t length = (size_t)bytes[2] | (size_t)bytes[3] << 8;
        if (!app_usb_midi_parse_configuration(bytes, length, &usb->interface)) {
            result = ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (result == ESP_OK) {
        result = usb_host_get_device_descriptor(usb->device, &device_descriptor);
    }
    if (result == ESP_OK) {
        result = usb_host_interface_claim(usb->client, usb->device,
                                           usb->interface.interface_number,
                                           usb->interface.alternate_setting);
        usb->claimed = result == ESP_OK;
    }
    if (result == ESP_OK && usb->interface.endpoint_in) {
        result = usb_host_transfer_alloc(usb->interface.in_packet_bytes, 0, &usb->receive);
    }
    if (result == ESP_OK && usb->interface.endpoint_out) {
        result = usb_host_transfer_alloc(usb->interface.out_packet_bytes, 0, &usb->transmit);
    }
    if (result != ESP_OK) {
        const esp_err_t closed = close_device(usb, milliseconds() + USB_TRANSITION_MS);
        return closed == ESP_OK ? result : closed;
    }
    status_lock(usb);
    usb->status.connected = true;
    usb->status.midi_in = usb->receive != NULL;
    usb->status.midi_out = usb->transmit != NULL;
    usb->status.vendor_id = device_descriptor->idVendor;
    usb->status.product_id = device_descriptor->idProduct;
    usb->status.last_error = ESP_OK;
    usb->status.state = APP_USB_STATE_HOST_CONNECTED;
    status_unlock(usb);
    atomic_store_explicit(&usb->transmit_ready, usb->transmit != NULL, memory_order_release);
    return ESP_OK;
}

static void service_receive(app_usb_midi_t *usb)
{
    if (usb->receive == NULL || usb->closing || usb->gone ||
            atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        return;
    }
    if (usb->rx_completed) {
        usb->rx_completed = false;
        if (usb->receive->status != USB_TRANSFER_STATUS_COMPLETED) {
            usb->gone = true;
            return;
        }
        const unsigned bytes = usb->receive->actual_num_bytes;
        if (bytes > usb->receive->data_buffer_size ||
                bytes > usb->interface.in_packet_bytes) {
            received_count(usb, 0, 1);
            usb->gone = true;
            app_note_router_midi_disconnect(usb->router);
            return;
        }
        app_usb_midi_packet_counts_t counts;
        (void)app_usb_midi_receive_packets(usb->router, usb->receive->data_buffer,
                                            bytes, &counts);
        received_count(usb, counts.accepted, counts.dropped);
    }
    if (!usb->rx_inflight &&
            !atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        usb->receive->num_bytes = usb->interface.in_packet_bytes;
        usb->receive->device_handle = usb->device;
        usb->receive->bEndpointAddress = usb->interface.endpoint_in;
        usb->receive->callback = receive_complete;
        usb->receive->context = usb;
        const esp_err_t result = usb_host_transfer_submit(usb->receive);
        usb->rx_inflight = result == ESP_OK;
        if (result != ESP_OK) {
            usb->gone = true;
            set_state(usb, APP_USB_MODE_MIDI_HOST, APP_USB_STATE_ERROR, result);
        }
    }
}

static void service_transmit(app_usb_midi_t *usb)
{
    if (usb->transmit == NULL || usb->closing || usb->gone || usb->tx_inflight ||
            atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        return;
    }
    if (usb->tx_completed) {
        usb->tx_completed = false;
        status_lock(usb);
        if (usb->transmit->status == USB_TRANSFER_STATUS_COMPLETED) {
            usb->status.sent_packets += (uint32_t)usb->transmit->actual_num_bytes / 4U;
        } else {
            usb->status.dropped_packets += (uint32_t)usb->transmit->num_bytes / 4U;
            atomic_store_explicit(&usb->transmit_panic, true, memory_order_release);
        }
        status_unlock(usb);
    }
    unsigned bytes = 0;
    if (atomic_exchange_explicit(&usb->transmit_panic, false, memory_order_acquire)) {
        (void)xQueueReset(usb->transmit_queue);
        const uint8_t panic[4] = {0x0b, 0xb0, 120, 0};
        memcpy(usb->transmit->data_buffer, panic, sizeof(panic));
        bytes = sizeof(panic);
    } else {
        midi_packet_t packet;
        while (bytes + sizeof(packet) <= usb->interface.out_packet_bytes &&
                xQueueReceive(usb->transmit_queue, &packet, 0) == pdTRUE) {
            memcpy(usb->transmit->data_buffer + bytes, packet.bytes, sizeof(packet.bytes));
            bytes += sizeof(packet.bytes);
        }
    }
    if (bytes && submit_transmit(usb, bytes) != ESP_OK) {
        usb->gone = true;
    }
}

static void settle_vendor_recovery(app_usb_midi_t *usb)
{
    if (!usb->vendor_recovery_pending || usb->library_installed ||
            atomic_load_explicit(&s_shutdown_requested, memory_order_acquire) ||
            milliseconds() < usb->vendor_retry_ms) {
        return;
    }
    usb->vendor_retry_ms = milliseconds() + 50U;
    esp_iris_usb_status_t iris;
    const esp_err_t observed = esp_iris_usb_get_status(&iris);
    if (observed != ESP_OK || iris.transition_pending) {
        return; /* A timed-out pause may still be executing; never steal PHY. */
    }
    const esp_err_t resumed = iris.enabled && !iris.phy_released ? ESP_OK :
        esp_iris_usb_set_enabled(true, USB_TRANSITION_MS);
    if (resumed == ESP_OK) {
        usb->vendor_recovery_pending = false;
        set_state(usb, APP_USB_MODE_VENDOR, APP_USB_STATE_ERROR,
                    usb->vendor_recovery_error);
    }
}

static void schedule_vendor_recovery(app_usb_midi_t *usb, esp_err_t error)
{
    usb->vendor_recovery_pending = true;
    usb->vendor_recovery_error = error;
    usb->vendor_retry_ms = 0;
    esp_iris_usb_status_t iris;
    const esp_err_t observed = esp_iris_usb_get_status(&iris);
    const bool vendor_confirmed = observed == ESP_OK && !iris.transition_pending &&
                                  iris.enabled && !iris.phy_released;
    set_state(usb, vendor_confirmed ? APP_USB_MODE_VENDOR : APP_USB_MODE_NONE,
                APP_USB_STATE_ERROR, error);
    settle_vendor_recovery(usb);
}

static void transition(app_usb_midi_t *usb, app_usb_mode_t target)
{
    if (atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
        return;
    }
    usb->vendor_recovery_pending = false;
    set_state(usb, usb->library_installed ? APP_USB_MODE_MIDI_HOST : APP_USB_MODE_VENDOR,
                APP_USB_STATE_SWITCHING, ESP_OK);
    if (target == APP_USB_MODE_MIDI_HOST) {
        if (usb->library_installed && usb->client != NULL && !usb->closing) {
            set_state(usb, APP_USB_MODE_MIDI_HOST,
                        usb->device ? APP_USB_STATE_HOST_CONNECTED : APP_USB_STATE_HOST_WAITING, ESP_OK);
            return;
        }
        if (usb->library_installed) {
            const esp_err_t stopped = stop_host(usb);
            if (stopped != ESP_OK) {
                set_state(usb, APP_USB_MODE_MIDI_HOST, APP_USB_STATE_ERROR, stopped);
                return;
            }
        }
        /* Only successful Iris acknowledgement transfers PHY ownership. Never
         * call TinyUSB uninstall or touch Iris private state from this app. */
        const esp_err_t paused = esp_iris_usb_set_enabled(false, USB_TRANSITION_MS);
        if (paused != ESP_OK) {
            schedule_vendor_recovery(usb, paused);
            return;
        }
        const esp_err_t result = start_host(usb);
        if (result == ESP_OK) {
            set_state(usb, APP_USB_MODE_MIDI_HOST, APP_USB_STATE_HOST_WAITING, ESP_OK);
            return;
        }
        const esp_err_t stopped = stop_host(usb);
        const esp_err_t restored = stopped == ESP_OK ?
            esp_iris_usb_set_enabled(true, USB_TRANSITION_MS) : stopped;
        set_state(usb, usb->library_installed ? APP_USB_MODE_MIDI_HOST :
                    restored == ESP_OK ? APP_USB_MODE_VENDOR : APP_USB_MODE_NONE,
                    APP_USB_STATE_ERROR, result);
        if (!usb->library_installed && restored != ESP_OK) {
            schedule_vendor_recovery(usb, result);
        }
    } else {
        const esp_err_t stopped = stop_host(usb);
        if (stopped != ESP_OK) {
            set_state(usb, APP_USB_MODE_MIDI_HOST, APP_USB_STATE_ERROR, stopped);
            return;
        }
        const esp_err_t resumed = esp_iris_usb_set_enabled(true, USB_TRANSITION_MS);
        set_state(usb, resumed == ESP_OK ? APP_USB_MODE_VENDOR : APP_USB_MODE_NONE,
                    resumed == ESP_OK ? APP_USB_STATE_VENDOR : APP_USB_STATE_ERROR, resumed);
        if (resumed != ESP_OK) {
            schedule_vendor_recovery(usb, resumed);
        }
    }
}

static void owner_task(void *context)
{
    app_usb_midi_t *usb = context;
    unsigned completed_request = 0;
    for (;;) {
        if (atomic_load_explicit(&s_shutdown_requested, memory_order_acquire)) {
            /* Terminal restart state: do not dispatch events or submit again
             * after emergency DMA reset. The CPU reset ends all lifetimes. */
            vTaskDelay(pdMS_TO_TICKS(USB_EVENT_WAIT_MS));
            continue;
        }
        const bool stopping = atomic_load_explicit(&usb->stopping, memory_order_acquire);
        const unsigned request = atomic_load_explicit(&usb->request_sequence, memory_order_acquire);
        if (request != completed_request || stopping) {
            const app_usb_mode_t target = stopping ? APP_USB_MODE_VENDOR :
                (app_usb_mode_t)atomic_load_explicit(&usb->requested_mode, memory_order_relaxed);
            transition(usb, target);
            completed_request = request;
            if (stopping && !usb->library_installed && usb->status.actual_mode == APP_USB_MODE_VENDOR) {
                app_note_router_set_midi_tx(usb->router, NULL, NULL);
                (void)esp_unregister_shutdown_handler(host_emergency_shutdown);
                atomic_store_explicit(&s_owner_claimed, false, memory_order_release);
                const SemaphoreHandle_t finished = usb->finished;
                (void)xSemaphoreGive(finished);
                vTaskDelete(NULL);
            }
        }
        settle_vendor_recovery(usb);
        if (usb->library_installed) {
            pump_events(usb, pdMS_TO_TICKS(USB_EVENT_WAIT_MS));
            if (usb->gone) {
                const esp_err_t closed = close_device(usb, milliseconds() + USB_TRANSITION_MS);
                set_state(usb, APP_USB_MODE_MIDI_HOST,
                            closed == ESP_OK ? APP_USB_STATE_HOST_WAITING : APP_USB_STATE_ERROR, closed);
            } else if (usb->candidate_address && usb->device == NULL && !usb->closing) {
                const esp_err_t opened = open_candidate(usb);
                if (opened != ESP_OK) {
                    set_state(usb, APP_USB_MODE_MIDI_HOST, APP_USB_STATE_HOST_WAITING, opened);
                }
            }
            service_receive(usb);
            service_transmit(usb);
        } else {
            vTaskDelay(pdMS_TO_TICKS(USB_EVENT_WAIT_MS));
        }
    }
}

bool app_usb_midi_create(app_note_router_t *router, app_usb_midi_t **out_handle)
{
    if (out_handle == NULL) {
        return false;
    }
    *out_handle = NULL;
    if (router == NULL) {
        return false;
    }
    if (atomic_exchange_explicit(&s_owner_claimed, true, memory_order_acq_rel)) {
        return false;
    }
    app_usb_midi_t *usb = calloc(1, sizeof(*usb));
    if (usb == NULL) {
        atomic_store_explicit(&s_owner_claimed, false, memory_order_release);
        return false;
    }
    usb->router = router;
    atomic_init(&usb->requested_mode, APP_USB_MODE_VENDOR);
    atomic_init(&usb->request_sequence, 0);
    atomic_init(&usb->stopping, false);
    atomic_init(&usb->transmit_ready, false);
    atomic_init(&usb->transmit_panic, false);
    usb->status.requested_mode = APP_USB_MODE_VENDOR;
    usb->status.actual_mode = APP_USB_MODE_VENDOR;
    usb->status.state = APP_USB_STATE_VENDOR;
    usb->status_lock = xSemaphoreCreateMutex();
    usb->finished = xSemaphoreCreateBinary();
    usb->transmit_queue = xQueueCreate(USB_TX_CAPACITY, sizeof(midi_packet_t));
    const esp_err_t shutdown_registered = esp_register_shutdown_handler(host_emergency_shutdown);
    if (usb->status_lock == NULL || usb->finished == NULL || usb->transmit_queue == NULL ||
            shutdown_registered != ESP_OK ||
            xTaskCreate(owner_task, "usb_midi", USB_OWNER_STACK_BYTES, usb,
                          USB_OWNER_PRIORITY, &usb->task) != pdPASS) {
        if (usb->status_lock) {
            vSemaphoreDelete(usb->status_lock);
        }
        if (usb->finished) {
            vSemaphoreDelete(usb->finished);
        }
        if (usb->transmit_queue) {
            vQueueDelete(usb->transmit_queue);
        }
        if (shutdown_registered == ESP_OK) {
            (void)esp_unregister_shutdown_handler(host_emergency_shutdown);
        }
        free(usb);
        atomic_store_explicit(&s_owner_claimed, false, memory_order_release);
        return false;
    }
    app_note_router_set_midi_tx(router, queue_transmit, usb);
    *out_handle = usb;
    return true;
}

bool app_usb_midi_request_mode(app_usb_midi_t *usb, app_usb_mode_t mode)
{
    if (usb == NULL || (mode != APP_USB_MODE_VENDOR && mode != APP_USB_MODE_MIDI_HOST) ||
            atomic_load_explicit(&usb->stopping, memory_order_acquire)) {
        return false;
    }
    status_lock(usb);
    usb->status.requested_mode = mode;
    status_unlock(usb);
    atomic_store_explicit(&usb->requested_mode, mode, memory_order_relaxed);
    atomic_fetch_add_explicit(&usb->request_sequence, 1U, memory_order_release);
    return true;
}

void app_usb_midi_get_status(app_usb_midi_t *usb, app_usb_midi_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (usb != NULL) {
        status_lock(usb);
        *status = usb->status;
        status_unlock(usb);
    }
}

bool app_usb_midi_shutdown(app_usb_midi_t **handle, uint32_t timeout_ms)
{
    if (handle == NULL || *handle == NULL) {
        return true;
    }
    app_usb_midi_t *usb = *handle;
    atomic_store_explicit(&usb->transmit_ready, false, memory_order_release);
    atomic_store_explicit(&usb->stopping, true, memory_order_release);
    if (xSemaphoreTake(usb->finished, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    vQueueDelete(usb->transmit_queue);
    vSemaphoreDelete(usb->status_lock);
    vSemaphoreDelete(usb->finished);
    free(usb);
    *handle = NULL;
    return true;
}
