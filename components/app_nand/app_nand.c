// SPDX-License-Identifier: Apache-2.0
#include "app_nand.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_blockdev.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "spi_nand_flash.h"

#define ESP_LITTLEFS_RAW_API
#include "lfs.h"

typedef struct {
    esp_blockdev_handle_t blockdev;
    spi_device_handle_t spi;
    bool bus_owned;
    bool mounted;
} nand_context_t;

/* One application mount owns SPI3 and the VFS until every project/inbox IO
 * user has stopped. Vibe Mode runs in a separate firmware image; its VFS is
 * not shared in memory. Existing on-media contents/geometry are shared. */
static nand_context_t nand;
static atomic_int mount_status = ATOMIC_VAR_INIT(ESP_ERR_INVALID_STATE);
static atomic_flag lifecycle = ATOMIC_FLAG_INIT;
static const char *TAG = "app_nand";

static int probe_read(const struct lfs_config *config, lfs_block_t block,
                      lfs_off_t offset, void *buffer, lfs_size_t size)
{
    const esp_blockdev_handle_t source = config->context;
    const uint64_t address = (uint64_t)block * config->block_size + offset;
    if (offset > config->block_size || size > config->block_size - offset ||
        address > source->geometry.disk_size || size > source->geometry.disk_size - address) {
        return LFS_ERR_IO;
    }
    return source->ops->read(source, buffer, size, address, size) == ESP_OK ? 0 : LFS_ERR_IO;
}

static int probe_write(const struct lfs_config *config, lfs_block_t block,
                       lfs_off_t offset, const void *buffer, lfs_size_t size)
{
    (void)config; (void)block; (void)offset; (void)buffer; (void)size;
    return LFS_ERR_IO;
}

static int probe_erase(const struct lfs_config *config, lfs_block_t block)
{
    (void)config; (void)block;
    return LFS_ERR_IO;
}

static int probe_sync(const struct lfs_config *config)
{
    (void)config;
    return 0;
}

static esp_err_t stored_geometry(esp_blockdev_handle_t source, uint64_t *out_bytes)
{
    const size_t page = source->geometry.read_size;
    if (source->ops == NULL || source->ops->read == NULL || page < 128 ||
        page > UINT32_MAX || (page & (page - 1U)) != 0 ||
        source->geometry.write_size != page || source->geometry.erase_size != page ||
        source->geometry.disk_size == 0 || source->geometry.disk_size % page != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Probe callbacks refuse write/erase. Unmount this raw read-only view
     * before VFS registration; never keep two mounted LittleFS instances. */
    struct lfs_config *config = calloc(1, sizeof(*config));
    lfs_t *probe = calloc(1, sizeof(*probe));
    struct lfs_fsinfo *info = calloc(1, sizeof(*info));
    if (config == NULL || probe == NULL || info == NULL) {
        free(info); free(probe); free(config);
        return ESP_ERR_NO_MEM;
    }
    *config = (struct lfs_config) {
        .context = source, .read = probe_read, .prog = probe_write,
        .erase = probe_erase, .sync = probe_sync,
        .read_size = page, .prog_size = page, .block_size = page,
        .block_count = 0, .cache_size = page, .lookahead_size = 16,
        .block_cycles = -1,
    };
    const int mounted = lfs_mount(probe, config);
    int result = mounted;
    if (mounted == 0) {
        result = lfs_fs_stat(probe, info);
        const int unmounted = lfs_unmount(probe);
        if (result == 0) result = unmounted;
    }
    esp_err_t error = result == LFS_ERR_NOMEM ? ESP_ERR_NO_MEM : result ? ESP_FAIL : ESP_OK;
    if (error == ESP_OK) {
        const uint64_t stored = (uint64_t)info->block_count * info->block_size;
        if (info->block_count == 0 || info->block_size != page ||
            stored > source->geometry.disk_size) {
            error = ESP_ERR_INVALID_SIZE;
        } else {
            *out_bytes = stored;
        }
    }
    free(info); free(probe); free(config);
    return error;
}

static esp_err_t release_resources(void)
{
    if (nand.blockdev != NULL && !nand.mounted) {
        const esp_err_t result = nand.blockdev->ops->release(nand.blockdev);
        if (result != ESP_OK) return result;
        nand.blockdev = NULL;
    }
    if (nand.spi != NULL) {
        const esp_err_t result = spi_bus_remove_device(nand.spi);
        if (result != ESP_OK) return result;
        nand.spi = NULL;
    }
    if (nand.bus_owned) {
        const esp_err_t result = spi_bus_free(BSP_NAND_SPI_HOST);
        if (result != ESP_OK) return result;
        nand.bus_owned = false;
    }
    return ESP_OK;
}

static esp_err_t release_pin(gpio_num_t pin)
{
    const gpio_config_t config = {
        .pin_bit_mask = UINT64_C(1) << pin, .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_set_level(pin, 1);
    if (result == ESP_OK) result = gpio_config(&config);
    return result;
}

static esp_err_t mount_volume(void)
{
    if (bsp_nand_flash_get_handle() != NULL) {
        /* A foreign raw BSP owner may hold the same device. Do not attach a
         * second driver, reinterpret its map or tear it down. */
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = release_resources();
    if (result != ESP_OK) return result;
    result = release_pin(BSP_NAND_HOLD);
    if (result == ESP_OK) result = release_pin(BSP_NAND_WP);
    if (result != ESP_OK) return result;
    const spi_bus_config_t bus = {
        .mosi_io_num = BSP_NAND_D, .miso_io_num = BSP_NAND_Q,
        .sclk_io_num = BSP_NAND_CLK, .quadhd_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC, .max_transfer_sz = 4096,
    };
    result = spi_bus_initialize(BSP_NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) return result;
    nand.bus_owned = true;
    const spi_device_interface_config_t device = {
        .clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ, .mode = 0,
        .spics_io_num = BSP_NAND_CS, .queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    result = spi_bus_add_device(BSP_NAND_SPI_HOST, &device, &nand.spi);
    if (result == ESP_OK) {
        spi_nand_flash_config_t config = {
            .device_handle = nand.spi, .gc_factor = 0,
            .io_mode = SPI_NAND_IO_MODE_SIO, .flags = SPI_DEVICE_HALFDUPLEX,
        };
        result = spi_nand_flash_init_with_layers(&config, &nand.blockdev);
    }
    uint64_t volume_bytes = 0;
    if (result == ESP_OK) result = stored_geometry(nand.blockdev, &volume_bytes);
    if (result == ESP_OK) {
        /* Dhara estimates may grow after bad-block map recovery. Preserve the
         * existing volume's persisted size, including a smaller older volume. */
        nand.blockdev->geometry.disk_size = volume_bytes;
        const esp_vfs_littlefs_conf_t config = {
            .base_path = APP_NAND_MOUNT_PATH, .blockdev = nand.blockdev,
            .format_if_mount_failed = false, .read_only = false,
            .dont_mount = false, .grow_on_mount = false,
        };
        result = esp_vfs_littlefs_register(&config);
        if (result != ESP_OK) nand.blockdev = NULL; /* Register consumes failure. */
    }
    if (result != ESP_OK) {
        const esp_err_t cleanup = release_resources();
        if (cleanup != ESP_OK) ESP_LOGE(TAG, "partial NAND cleanup retained: %s", esp_err_to_name(cleanup));
        return result;
    }
    nand.mounted = true;
    ESP_LOGI(TAG, "existing NAND LittleFS mounted; preserved geometry=%llu",
             (unsigned long long)volume_bytes);
    return ESP_OK;
}

esp_err_t app_nand_mount(void)
{
    if (atomic_flag_test_and_set_explicit(&lifecycle, memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_OK;
    if (!nand.mounted) {
        atomic_store_explicit(&mount_status, ESP_ERR_INVALID_STATE, memory_order_release);
        result = mount_volume();
    }
    atomic_store_explicit(&mount_status, result == ESP_ERR_INVALID_STATE ? ESP_FAIL : result,
                           memory_order_release);
    atomic_flag_clear_explicit(&lifecycle, memory_order_release);
    return result;
}

esp_err_t app_nand_status(void)
{
    return atomic_load_explicit(&mount_status, memory_order_acquire);
}

esp_err_t app_nand_info(uint64_t *total_bytes, uint64_t *used_bytes)
{
    if (total_bytes == NULL || used_bytes == NULL) return ESP_ERR_INVALID_ARG;
    if (app_nand_status() != ESP_OK || !nand.mounted) return ESP_ERR_INVALID_STATE;
    size_t total = 0, used = 0;
    const esp_err_t result = esp_littlefs_blockdev_info(nand.blockdev, &total, &used);
    if (result == ESP_OK) { *total_bytes = total; *used_bytes = used; }
    return result;
}

esp_err_t app_nand_unmount(void)
{
    if (atomic_flag_test_and_set_explicit(&lifecycle, memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    atomic_store_explicit(&mount_status, ESP_ERR_INVALID_STATE, memory_order_release);
    esp_err_t result = ESP_OK;
    if (nand.mounted) {
        result = esp_vfs_littlefs_unregister_blockdev(nand.blockdev);
        if (result == ESP_OK) { nand.mounted = false; nand.blockdev = NULL; }
    }
    if (result == ESP_OK) result = release_resources();
    atomic_store_explicit(&mount_status, result == ESP_OK ? ESP_ERR_INVALID_STATE : result,
                           memory_order_release);
    atomic_flag_clear_explicit(&lifecycle, memory_order_release);
    return result;
}
