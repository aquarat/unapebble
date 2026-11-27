/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The Lab: experiments run on demand, each a list of steps, with a breadcrumb and a marker on
// screen before each so that a step that resets the watch shows on the next boot.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/kernel/compiler.h>

// The GUI region of the eMMC, the only one the Lab writes (UNA's layout: UPD from 0, SYS from
// 204800, GUI from 409600, USR from 614400), in 512-byte blocks
#define LAB_GUI_FIRST 409600U
#define LAB_GUI_END   614400U
// The window the eMMC builds keep their storage in: the first 32 MiB of GUI
#define LAB_WINDOW_FIRST  LAB_GUI_FIRST
#define LAB_WINDOW_BLOCKS 65536U
// The Lab's own scratch blocks, past that window, for its write tests
#define LAB_SCRATCH_LBA    600000U
#define LAB_SCRATCH_BLOCKS 256U
// The update drive
#define LAB_UPD_FIRST  0U
#define LAB_UPD_BLOCKS 204800U

// Offsets in the window of the eMMC builds (pebble/boards/unawatch.overlay)
#define LAB_B_BANK0_OFFSET 0x0000000U
#define LAB_B_BANK1_OFFSET 0x0400000U
#define LAB_B_BANK_SIZE    0x0400000U
#define LAB_B_SCRATCH      0x0900000U
#define LAB_B_DEBUG_DB     0x0d00000U
#define LAB_B_DEBUG_SIZE   0x0080000U
#define LAB_B_FS_OFFSET    0x1000000U
#define LAB_B_FS_SIZE      0x1000000U

// A step returns 0, a negative errno, which ends the experiment, or LAB_SKIP
#define LAB_SKIP 1

typedef int (*lab_step_fn)(void);

struct lab_step {
  const char *name;
  lab_step_fn fn;
};

struct lab_experiment {
  const char *title;
  const char *subtitle;
  const struct lab_step *steps;
  size_t num_steps;
  // Run after the last step or a failed one, also when the window is left
  void (*cleanup)(void);
  // When set, shown first, and the steps only run on a second Select
  const char *confirm;
};

// Output on the result window
void lab_printf(const char *fmt, ...) PBL_FORMAT_PRINTF(1, 2);
void lab_hex(const char *label, const void *buf, size_t len);

// A buffer of the app's heap, freed when the experiment ends
void *lab_alloc(size_t size);

// Whether blocks are in the GUI region, the only ones the Lab writes
bool lab_gui_writable(uint32_t lba, uint32_t count);

// The eMMC through the MMC class, writes refused outside GUI
int lab_mmc_read(uint32_t lba, void *buf, uint32_t count);
int lab_mmc_write(uint32_t lba, const void *buf, uint32_t count);

// The kernel clock of the SDMMC, for the raw experiments and the driver: false for HSI48, true
// for PLL1 Q as the eMMC builds have it
int lab_emmc_clock_select(bool pll1_q);
bool lab_emmc_clock_is_pll1_q(void);

// The experiments, by group
extern const struct lab_experiment lab_exp_boot;
extern const struct lab_experiment lab_exp_clock_show;
extern const struct lab_experiment lab_exp_clock_b;
extern const struct lab_experiment lab_exp_clock_hsi48;
extern const struct lab_experiment lab_exp_emmc_raw;
extern const struct lab_experiment lab_exp_emmc_driver;
extern const struct lab_experiment lab_exp_emmc_supply_cut;
extern const struct lab_experiment lab_exp_nor;
extern const struct lab_experiment lab_exp_fat;
extern const struct lab_experiment lab_exp_copy;
extern const struct lab_experiment lab_exp_b_storage;
extern const struct lab_experiment lab_exp_b_logs;
extern const struct lab_experiment lab_exp_battery;
extern const struct lab_experiment lab_exp_test_fault;
extern const struct lab_experiment lab_exp_test_assert;
extern const struct lab_experiment lab_exp_test_watchdog;
extern const struct lab_experiment lab_exp_test_reset;
extern const struct lab_experiment lab_exp_test_power_off;

// All experiments, in menu order; a breadcrumb's area is PBL_BT_LAB + the index here
extern const struct lab_experiment *const lab_experiments[];
extern const size_t lab_num_experiments;

// The boot report: what the run before left
void lab_boot_report(void);
// One line on how the run before ended, for the menu
const char *lab_boot_summary(void);
// Where the run before last was: its last breadcrumb but those of the reboot path; its uptime
uint32_t lab_previous_place(uint32_t *ms);
// The name of a breadcrumb, into buf
void lab_crumb_name(uint32_t crumb, char *buf, size_t size);
