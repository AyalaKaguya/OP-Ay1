// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Compiled only into project_files.c in the native test target. Source DSP,
 * sampler codecs and fixture setup continue to use their real libc functions. */
size_t project_test_fwrite(const void *data, size_t size, size_t count, FILE *file);
void project_test_fail_next_write(void);
void project_test_hold_writes(bool held);
bool project_test_write_is_held(void);
#define fwrite project_test_fwrite
