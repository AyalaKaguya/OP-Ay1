// SPDX-License-Identifier: Apache-2.0
/* White-box resource fault injection of the real production owner. Only the
 * USB/FreeRTOS backend is fake; no device is accessed and no duplicate mode
 * state machine is substituted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../components/app_usb_midi/usb_midi_host.c"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "midi_host_lifecycle:%d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

struct fake_queue { unsigned count; unsigned capacity; unsigned item_size; uint8_t bytes[256]; };
struct fake_semaphore { unsigned count; };
struct fake_client { int marker; };
struct fake_device { int marker; };
static struct fake_client client_instance;
static struct fake_device device_instance;
static bool iris_enabled = true;
static bool iris_pending;
static bool bus_clock_enabled;
static bool host_installed;
static bool client_registered;
static bool device_open;
static bool interface_claimed;
static bool hold_completions;
static bool host_free_pending;
static bool device_free_ready;
static int fail_stage;
static int install_stage;
static unsigned live_transfers;
static unsigned transfer_releases;
static unsigned submitted_tx;
static uint8_t last_transmit[512];
static size_t last_transmit_size;
static usb_transfer_t *pending_rx;
static usb_transfer_t *pending_tx;
static bool cancel_rx;
static bool cancel_tx;
static uint64_t clock_us;
static unsigned shutdown_steps;
static unsigned shutdown_routes;
static unsigned resume_attempts;
static void (*registered_shutdown)(void);
static float scratch[1024];
static const uint8_t configuration[] = {
    9, 2, 49, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 1, 3, 0, 0,
    7, 0x24, 1, 0, 1, 37, 0,
    7, 5, 0x81, 2, 64, 0, 0,
    5, 0x25, 1, 1, 1,
    7, 5, 0x01, 2, 64, 0, 0,
    5, 0x25, 1, 1, 3,
};

static bool fail_next(void) { return ++install_stage == fail_stage; }

QueueHandle_t xQueueCreate(unsigned count, unsigned bytes)
{
    CHECK(count * bytes <= sizeof(((struct fake_queue *)0)->bytes));
    struct fake_queue *queue = calloc(1, sizeof(*queue));
    if (queue) { queue->capacity = count; queue->item_size = bytes; }
    return queue;
}
int xQueueSend(QueueHandle_t q, const void *p, TickType_t wait)
{
    (void)wait;
    if (q->count == q->capacity) { return 0; }
    memcpy(q->bytes + q->count++ * q->item_size, p, q->item_size);
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *p, TickType_t wait)
{
    (void)wait;
    if (!q->count) { return 0; }
    memcpy(p, q->bytes, q->item_size);
    memmove(q->bytes, q->bytes + q->item_size, --q->count * q->item_size);
    return pdTRUE;
}
int xQueueReset(QueueHandle_t q) { q->count = 0; return pdTRUE; }
void vQueueDelete(QueueHandle_t q) { free(q); }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct fake_semaphore *s = malloc(sizeof(*s));
    if (s) { s->count = 1; }
    return s;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    struct fake_semaphore *s = malloc(sizeof(*s));
    if (s) { s->count = 0; }
    return s;
}
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    if (s->count) { --s->count; return pdTRUE; }
    clock_us += (uint64_t)wait * 1000;
    return 0;
}
int xSemaphoreGive(SemaphoreHandle_t s) { ++s->count; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
int xTaskCreate(void (*entry)(void *), const char *name, unsigned bytes, void *context,
                  unsigned priority, TaskHandle_t *task)
{
    CHECK(entry != NULL && name != NULL && context != NULL && bytes == 6144 && priority == 4);
    *task = context; /* Tests call the real static handlers under deterministic scheduling. */
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(TickType_t ticks) { clock_us += (uint64_t)ticks * 1000; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_us; }
esp_err_t esp_register_shutdown_handler(void (*handler)(void))
{
    CHECK(registered_shutdown == NULL);
    registered_shutdown = handler;
    return ESP_OK;
}
esp_err_t esp_unregister_shutdown_handler(void (*handler)(void))
{
    CHECK(registered_shutdown == handler);
    registered_shutdown = NULL;
    return ESP_OK;
}
void usb_dwc_ll_gahbcfg_dis_global_intr(void *hardware)
{
    CHECK(hardware != NULL && shutdown_steps == 0); ++shutdown_steps;
}
void usb_utmi_ll_reset_register(void) { CHECK(shutdown_steps == 1); ++shutdown_steps; }
void usb_utmi_ll_enable_bus_clock(bool enabled) { (void)enabled; CHECK(false); }
bool _usb_utmi_ll_bus_clock_is_enabled(void) { return bus_clock_enabled; }
void esp_rom_route_intr_matrix(int cpu, uint32_t source, uint32_t interrupt)
{
    CHECK(cpu >= 0 && cpu < 2 && source == ETS_USB_OTGHS_INTR_SOURCE && interrupt == ETS_INVALID_INUM);
    ++shutdown_routes;
}

esp_err_t esp_iris_usb_set_enabled(bool enabled, uint32_t timeout_ms)
{
    CHECK(timeout_ms == 2000);
    CHECK(!host_installed); /* Owner may never resume Iris over a Host stack. */
    if (enabled) { ++resume_attempts; }
    if (iris_pending) { return ESP_ERR_INVALID_STATE; }
    if (fail_stage == -1 && !enabled) { return ESP_ERR_TIMEOUT; }
    if (fail_stage == -3 && !enabled) { iris_pending = true; return ESP_ERR_TIMEOUT; }
    if (fail_stage == -2 && enabled) { return ESP_ERR_NO_MEM; }
    iris_enabled = enabled;
    return ESP_OK;
}
esp_err_t esp_iris_usb_get_status(esp_iris_usb_status_t *status)
{
    CHECK(status != NULL);
    status->enabled = iris_enabled;
    status->transition_pending = iris_pending;
    status->phy_released = !iris_enabled && !iris_pending;
    status->last_error = ESP_OK;
    return ESP_OK;
}
esp_err_t usb_host_install(const usb_host_config_t *config)
{
    CHECK(!iris_enabled && !host_installed && config->peripheral_map == 1);
    if (fail_next()) { return ESP_ERR_NO_MEM; }
    host_installed = true;
    bus_clock_enabled = true;
    return ESP_OK;
}
esp_err_t usb_host_uninstall(void)
{
    CHECK(host_installed && !device_open && !client_registered && !live_transfers);
    host_installed = false;
    bus_clock_enabled = false;
    return ESP_OK;
}
esp_err_t usb_host_lib_handle_events(TickType_t wait, uint32_t *events)
{
    clock_us += (uint64_t)wait * 1000;
    *events = 0;
    if (host_free_pending) { *events = USB_HOST_LIB_EVENT_FLAGS_ALL_FREE; host_free_pending = false; device_free_ready = true; }
    return ESP_OK;
}
esp_err_t usb_host_device_free_all(void)
{
    CHECK(!client_registered && !device_open);
    if (!device_free_ready) { host_free_pending = true; return ESP_ERR_NOT_FINISHED; }
    return ESP_OK;
}
esp_err_t usb_host_client_register(const usb_host_client_config_t *config, usb_host_client_handle_t *out)
{
    CHECK(host_installed && !client_registered && config->max_num_event_msg == 8);
    if (fail_next()) { return ESP_ERR_NO_MEM; }
    client_registered = true; *out = &client_instance;
    return ESP_OK;
}
esp_err_t usb_host_client_deregister(usb_host_client_handle_t client)
{
    CHECK(client == &client_instance && client_registered && !device_open);
    client_registered = false;
    return ESP_OK;
}
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t client, TickType_t wait)
{
    CHECK(client == &client_instance && client_registered);
    clock_us += (uint64_t)wait * 1000;
    if (!hold_completions && pending_rx && cancel_rx) {
        usb_transfer_t *t = pending_rx; pending_rx = NULL; cancel_rx = false;
        t->status = USB_TRANSFER_STATUS_CANCELLED; t->actual_num_bytes = 0; t->callback(t);
    }
    if (!hold_completions && pending_tx) {
        usb_transfer_t *t = pending_tx; pending_tx = NULL;
        t->status = cancel_tx ? USB_TRANSFER_STATUS_CANCELLED : USB_TRANSFER_STATUS_COMPLETED;
        cancel_tx = false; t->actual_num_bytes = t->num_bytes; t->callback(t);
    }
    return ESP_OK;
}
esp_err_t usb_host_device_open(usb_host_client_handle_t client, uint8_t address, usb_device_handle_t *out)
{
    CHECK(client == &client_instance && address == 1 && !device_open);
    if (fail_next()) { return ESP_ERR_NO_MEM; }
    *out = &device_instance; device_open = true; device_free_ready = false;
    return ESP_OK;
}
esp_err_t usb_host_device_close(usb_host_client_handle_t client, usb_device_handle_t device)
{
    CHECK(client == &client_instance && device == &device_instance && device_open && !interface_claimed);
    CHECK(pending_rx == NULL && pending_tx == NULL);
    device_open = false; return ESP_OK;
}
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t device, const usb_config_desc_t **out)
{
    CHECK(device == &device_instance);
    *out = (const usb_config_desc_t *)configuration; return ESP_OK;
}
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t device, const usb_device_desc_t **out)
{
    static const usb_device_desc_t description = {.idVendor = 0x1234, .idProduct = 0x5678};
    CHECK(device == &device_instance); *out = &description; return ESP_OK;
}
esp_err_t usb_host_interface_claim(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t number, uint8_t alt)
{
    CHECK(client == &client_instance && device == &device_instance && number == 0 && alt == 0);
    if (fail_next()) { return ESP_ERR_NO_MEM; }
    interface_claimed = true; return ESP_OK;
}
esp_err_t usb_host_interface_release(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t number)
{
    CHECK(client == &client_instance && device == &device_instance && number == 0 && interface_claimed);
    CHECK(pending_rx == NULL && pending_tx == NULL);
    interface_claimed = false; return ESP_OK;
}
esp_err_t usb_host_endpoint_halt(usb_device_handle_t device, uint8_t endpoint)
{
    CHECK(device == &device_instance && (endpoint == 0x81 || endpoint == 1)); return ESP_OK;
}
esp_err_t usb_host_endpoint_flush(usb_device_handle_t device, uint8_t endpoint)
{
    CHECK(device == &device_instance);
    if (endpoint == 0x81) { cancel_rx = true; } else { cancel_tx = true; }
    return ESP_OK;
}
esp_err_t usb_host_transfer_alloc(size_t size, int isoc, usb_transfer_t **out)
{
    CHECK(size == 64 && isoc == 0);
    if (fail_next()) { return ESP_ERR_NO_MEM; }
    usb_transfer_t *t = calloc(1, sizeof(*t)); CHECK(t != NULL);
    t->data_buffer = malloc(size); CHECK(t->data_buffer != NULL);
    t->data_buffer_size = size; *out = t; ++live_transfers; return ESP_OK;
}
esp_err_t usb_host_transfer_free(usb_transfer_t *t)
{
    CHECK(t != NULL && t != pending_rx && t != pending_tx && live_transfers);
    free(t->data_buffer); free(t); --live_transfers; ++transfer_releases; return ESP_OK;
}
esp_err_t usb_host_transfer_submit(usb_transfer_t *t)
{
    CHECK(interface_claimed && !iris_enabled && t->num_bytes > 0 &&
          (size_t)t->num_bytes <= t->data_buffer_size);
    if (t->bEndpointAddress == 0x81) { CHECK(pending_rx == NULL); pending_rx = t; }
    else {
        CHECK(t->bEndpointAddress == 1 && pending_tx == NULL);
        pending_tx = t; ++submitted_tx;
        last_transmit_size = (size_t)t->num_bytes;
        memcpy(last_transmit, t->data_buffer, last_transmit_size);
    }
    return ESP_OK;
}

static app_usb_midi_t *create_owner(app_note_router_t *router)
{
    app_usb_midi_t *usb = NULL;
    CHECK(app_usb_midi_create(router, &usb) && usb != NULL);
    return usb;
}

static void destroy_owner(app_usb_midi_t *usb)
{
    transition(usb, APP_USB_MODE_VENDOR);
    CHECK(!host_installed && iris_enabled && !device_open && !live_transfers);
    app_note_router_set_midi_tx(usb->router, NULL, NULL);
    CHECK(esp_unregister_shutdown_handler(host_emergency_shutdown) == ESP_OK);
    vQueueDelete(usb->transmit_queue); vSemaphoreDelete(usb->finished); vSemaphoreDelete(usb->status_lock);
    free(usb); atomic_store(&s_owner_claimed, false);
}

static void connect_keyboard(app_usb_midi_t *usb)
{
    const usb_host_client_event_msg_t event = {.event = USB_HOST_CLIENT_EVENT_NEW_DEV,
                                               .new_dev = {.address = 1}};
    client_event(&event, usb); CHECK(usb->candidate_address == 1);
    CHECK(open_candidate(usb) == ESP_OK);
    CHECK(usb->status.connected && usb->status.vendor_id == 0x1234 && usb->status.midi_in && usb->status.midi_out);
}

static void test_failures(app_note_router_t *router)
{
    for (int stage = 1; stage <= 2; ++stage) {
        fail_stage = stage; install_stage = 0;
        app_usb_midi_t *usb = create_owner(router);
        transition(usb, APP_USB_MODE_MIDI_HOST);
        CHECK(usb->status.state == APP_USB_STATE_ERROR && usb->status.actual_mode == APP_USB_MODE_VENDOR);
        CHECK(!host_installed && !client_registered && iris_enabled);
        fail_stage = 0; destroy_owner(usb);
    }
    for (int stage = 3; stage <= 6; ++stage) {
        fail_stage = stage; install_stage = 0;
        app_usb_midi_t *usb = create_owner(router);
        transition(usb, APP_USB_MODE_MIDI_HOST); CHECK(host_installed && client_registered);
        usb->candidate_address = 1; CHECK(open_candidate(usb) != ESP_OK);
        CHECK(!device_open && !interface_claimed && !live_transfers);
        fail_stage = 0; destroy_owner(usb);
    }
    fail_stage = -1; install_stage = 0;
    app_usb_midi_t *usb = create_owner(router);
    transition(usb, APP_USB_MODE_MIDI_HOST);
    CHECK(usb->status.state == APP_USB_STATE_ERROR && usb->status.last_error == ESP_ERR_TIMEOUT);
    CHECK(!host_installed && iris_enabled);
    fail_stage = 0; destroy_owner(usb);
    puts("midi host: pause/install/client/open/claim/buffer rollback PASS");
}

static void test_data_and_teardown(app_note_router_t *router, app_synth_t *synth)
{
    install_stage = 0;
    app_usb_midi_t *usb = create_owner(router);
    app_usb_midi_t *duplicate = NULL; CHECK(!app_usb_midi_create(router, &duplicate));
    transition(usb, APP_USB_MODE_MIDI_HOST); connect_keyboard(usb);
    service_receive(usb); CHECK(pending_rx && usb->rx_inflight);
    const uint8_t on[4] = {9, 0x90, 60, 100};
    memcpy(pending_rx->data_buffer, on, sizeof(on));
    pending_rx->actual_num_bytes = 4; pending_rx->status = USB_TRANSFER_STATUS_COMPLETED;
    usb_transfer_t *received = pending_rx; pending_rx = NULL; receive_complete(received);
    service_receive(usb); CHECK(usb->status.received_packets == 1 && pending_rx);
    app_note_router_status_t ownership; app_note_router_get_status(router, &ownership);
    CHECK(ownership.midi_notes == 1);

    CHECK(app_note_router_touch_note_on(router, 64, .8f));
    service_transmit(usb); CHECK(pending_tx && last_transmit[1] == 0x90 && submitted_tx > 0);
    app_note_router_touch_note_off(router, 64);
    pump_events(usb, 1); service_transmit(usb); CHECK(last_transmit[1] == 0x80);
    pump_events(usb, 1); service_transmit(usb);
    CHECK(last_transmit_size == 4);
    for (unsigned i = 0; i <= USB_TX_CAPACITY; ++i) { (void)queue_transmit(usb, on); }
    CHECK(atomic_load(&usb->transmit_panic));
    service_transmit(usb); CHECK(last_transmit[1] == 0xb0 && last_transmit[2] == 120);
    pump_events(usb, 1); service_transmit(usb);

    hold_completions = true;
    const unsigned before_releases = transfer_releases;
    CHECK(close_device(usb, milliseconds() + 20) == ESP_ERR_TIMEOUT);
    CHECK(live_transfers == 2 && transfer_releases == before_releases && device_open);
    hold_completions = false;
    CHECK(close_device(usb, milliseconds() + 20) == ESP_OK);
    CHECK(!device_open && !live_transfers);
    app_note_router_get_status(router, &ownership); CHECK(ownership.midi_notes == 0);
    app_synth_render(synth, scratch, 512);
    destroy_owner(usb);
    puts("midi host: real RX/TX routing, TX overflow panic, in-flight timeout retention PASS");
}

static void test_warm_reset_and_resume_failure(app_note_router_t *router)
{
    install_stage = 0;
    app_usb_midi_t *usb = create_owner(router);
    transition(usb, APP_USB_MODE_MIDI_HOST);
    fail_stage = -2;
    transition(usb, APP_USB_MODE_VENDOR);
    CHECK(!host_installed && !iris_enabled && usb->status.actual_mode == APP_USB_MODE_NONE);
    CHECK(usb->status.last_error == ESP_ERR_NO_MEM);
    fail_stage = 0;
    vTaskDelay(50); settle_vendor_recovery(usb);
    CHECK(iris_enabled && !usb->vendor_recovery_pending);
    transition(usb, APP_USB_MODE_VENDOR); CHECK(iris_enabled);
    transition(usb, APP_USB_MODE_MIDI_HOST);
    CHECK(registered_shutdown != NULL); registered_shutdown();
    CHECK(shutdown_steps == 2 && shutdown_routes == 2 && bus_clock_enabled &&
          !atomic_load(&s_host_hardware_active) && atomic_load(&s_shutdown_requested));
    service_receive(usb); service_transmit(usb);
    registered_shutdown(); CHECK(shutdown_steps == 2 && shutdown_routes == 2);
    atomic_store(&s_shutdown_requested, false); /* Simulate a fresh CPU boot for the next test. */
    destroy_owner(usb);
    puts("midi host: truthful resume failure and bounded S31 DMA shutdown PASS");
}

static void test_late_pause_recovery(app_note_router_t *router)
{
    install_stage = 0; fail_stage = -3;
    app_usb_midi_t *usb = create_owner(router);
    const unsigned attempts_before = resume_attempts;
    transition(usb, APP_USB_MODE_MIDI_HOST);
    CHECK(iris_pending && !host_installed && usb->vendor_recovery_pending);
    CHECK(usb->status.actual_mode == APP_USB_MODE_NONE && usb->status.last_error == ESP_ERR_TIMEOUT);
    vTaskDelay(100); settle_vendor_recovery(usb);
    CHECK(resume_attempts == attempts_before && usb->vendor_recovery_pending);
    /* Iris finishes the previously timed-out pause after the request already
     * returned. No user gesture is required to recover management USB. */
    iris_pending = false; iris_enabled = false; fail_stage = 0;
    vTaskDelay(100); settle_vendor_recovery(usb);
    CHECK(iris_enabled && !host_installed && !usb->vendor_recovery_pending);
    CHECK(usb->status.actual_mode == APP_USB_MODE_VENDOR &&
          usb->status.last_error == ESP_ERR_TIMEOUT && usb->status.state == APP_USB_STATE_ERROR);
    destroy_owner(usb);
    puts("midi host: executing pause timeout settles and restores Vendor automatically PASS");
}

static void test_clock_off_shutdown_window(void)
{
    bus_clock_enabled = false; shutdown_steps = 0; shutdown_routes = 0;
    atomic_store(&s_host_hardware_active, true);
    host_emergency_shutdown();
    CHECK(shutdown_steps == 0 && shutdown_routes == 2);
    atomic_store(&s_shutdown_requested, false);
    puts("midi host: startup/teardown clock-off shutdown skips unsafe MMIO PASS");
}

int main(void)
{
    app_synth_t *synth = app_synth_create(48000);
    app_note_router_t *router = app_note_router_create(synth);
    CHECK(synth != NULL && router != NULL);
    test_failures(router);
    test_data_and_teardown(router, synth);
    test_late_pause_recovery(router);
    test_warm_reset_and_resume_failure(router);
    test_clock_off_shutdown_window();
    app_note_router_destroy(router); app_synth_destroy(synth);
    puts("midi production host lifecycle tests: PASS");
    return 0;
}
