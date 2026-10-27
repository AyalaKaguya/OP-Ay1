// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_NAND_MOUNT_PATH "/nand"
#define APP_NAND_PROJECT_ROOT "/nand/apps/op-ay1"
#define APP_NAND_INBOX_PATH "/nand/apps/op-ay1/inbox"

/* Only the project IO worker calls mount/info/unmount. Status is nonblocking
 * and safe before startup: INVALID_STATE means not mounted or transitioning.
 * No API formats, grows, repairs or erases the filesystem. */
esp_err_t app_nand_mount(void);
esp_err_t app_nand_status(void);
esp_err_t app_nand_info(uint64_t *total_bytes, uint64_t *used_bytes);
/* All VFS users/streams must have stopped first. Failure retains ownership
 * for retry, including outstanding SPI resources. */
esp_err_t app_nand_unmount(void);

#ifdef __cplusplus
}
#endif
