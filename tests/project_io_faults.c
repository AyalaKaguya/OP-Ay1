// SPDX-License-Identifier: Apache-2.0
#include "project_io_faults.h"
#undef fwrite
#include <errno.h>
#include <stdatomic.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void pause_one(void) { Sleep(1); }
#else
#include <time.h>
static void pause_one(void) { const struct timespec t = {0, 1000000L}; nanosleep(&t, NULL); }
#endif

static atomic_bool fail_write;
static atomic_bool hold_write;
static atomic_bool write_held;

void project_test_fail_next_write(void) { atomic_store(&fail_write, true); }
void project_test_hold_writes(bool held) { atomic_store(&hold_write, held); }
bool project_test_write_is_held(void) { return atomic_load(&write_held); }
size_t project_test_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    if (atomic_load(&hold_write)) {
        atomic_store(&write_held, true);
        while (atomic_load(&hold_write)) pause_one();
        atomic_store(&write_held, false);
    }
    if (atomic_exchange(&fail_write, false)) { errno = ENOSPC; return 0; }
    return fwrite(data, size, count, file);
}
