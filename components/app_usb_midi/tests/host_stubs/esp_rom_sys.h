// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
void esp_rom_route_intr_matrix(int cpu, uint32_t source, uint32_t interrupt);
