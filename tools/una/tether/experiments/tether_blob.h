/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * For experiments the tether stub runs. Write blob_main(); blob_crt0.c
 * clears .bss, keeps the API in `tether` and calls it. Experiments read; one
 * that writes says so in its name and its first line of output, and never
 * writes the eMMC outside the GUI region (tether_emmc_write_allowed).
 */

#pragma once

#include "tether_api.h"
#include "tether_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern const struct tether_api *tether;

/* The experiment: its return value comes back as r0 */
uint32_t blob_main(const uint32_t *args, uint32_t nargs);

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

#define tprintf(...) tether->printf(__VA_ARGS__)

/* GPIO port n (0 = A) */
#define GPIO_PORT(n) (0x42020000U + (uint32_t)(n) * 0x400U)

/* The GUI region of UNA's eMMC layout, which nothing else uses: sectors 409600 to 614399 */
#define TETHER_EMMC_GUI_FIRST 409600U
#define TETHER_EMMC_GUI_END   614400U

static inline bool tether_emmc_write_allowed(uint32_t sector, uint32_t count) {
  return sector >= TETHER_EMMC_GUI_FIRST && count <= TETHER_EMMC_GUI_END - sector;
}

static inline void tether_delay_ms(uint32_t ms) {
  while (ms-- != 0U) {
    tether->delay_us(1000U);
    tether->watchdog_feed();
  }
}
