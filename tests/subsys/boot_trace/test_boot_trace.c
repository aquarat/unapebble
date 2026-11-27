/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>
#include <string.h>

#include <pbl/boot_trace/boot_trace.h>

#include <clar.h>

// The RAM the record lives in, and the backup registers, across the "resets" of a test
uint8_t test_boot_trace_ram[0x400] __attribute__((aligned(8)));
extern uint32_t test_boot_trace_reset_flags;

static uint32_t s_bkp[32];
static uint64_t s_uptime_ms;

bool pbl_kernel_is_running(void) {
  return true;
}

uint64_t pbl_uptime_ms(void) {
  return s_uptime_ms;
}

void retained_write(uint8_t id, uint32_t value) {
  cl_assert(id >= 24U && id < 32U);
  s_bkp[id] = value;
}

uint32_t retained_read(uint8_t id) {
  return s_bkp[id];
}

void test_boot_trace__initialize(void) {
  memset(test_boot_trace_ram, 0xA5, sizeof(test_boot_trace_ram));
  memset(s_bkp, 0, sizeof(s_bkp));
  test_boot_trace_reset_flags = 0U;
  s_uptime_ms = 0U;
}

void test_boot_trace__first_boot_finds_nothing(void) {
  const struct pbl_boot_trace_boot *boot;

  pbl_boot_trace_init();
  boot = pbl_boot_trace_boot_info();

  cl_assert(!boot->bkp_valid);
  cl_assert(!boot->ram_valid);
  cl_assert_equal_i(pbl_boot_trace_current()->boot_count, 1);
  cl_assert_equal_s(pbl_boot_trace_current()->image, "test");
  // The kernel's start is the first breadcrumb
  cl_assert_equal_i(s_bkp[30], PBL_BOOT_TRACE_CRUMB(PBL_BT_KERNEL, PBL_BT_KERNEL_START));
  cl_assert_equal_i(s_bkp[31] >> 24, 0xB7);
  cl_assert_equal_i((s_bkp[31] >> 20) & 0xFU, 1);
}

void test_boot_trace__next_boot_keeps_the_run_before(void) {
  const struct pbl_boot_trace_boot *boot;

  pbl_boot_trace_init();
  s_uptime_ms = 31500U;
  pbl_boot_trace_crumb(PBL_BT_PFS, PBL_BT_PFS_FORMAT);
  pbl_boot_trace_note(PBL_BT_NOTE_RES_CRC, 0x12345678U);
  s_bkp[25] = 0x11U;

  // Reset: an IWDG
  test_boot_trace_reset_flags = (1UL << 29) | (1UL << 26);
  pbl_boot_trace_init();
  boot = pbl_boot_trace_boot_info();

  cl_assert(boot->bkp_valid);
  cl_assert(boot->ram_valid);
  cl_assert(boot->ram_current);
  cl_assert_equal_i(boot->reset_flags, (1UL << 29) | (1UL << 26));
  cl_assert_equal_i(boot->bkp_crumb, PBL_BOOT_TRACE_CRUMB(PBL_BT_PFS, PBL_BT_PFS_FORMAT));
  cl_assert_equal_i(boot->previous.boot_count, 1);
  cl_assert_equal_i(boot->previous.trail_count, 2);
  cl_assert_equal_i(boot->previous.trail[1].crumb,
                    PBL_BOOT_TRACE_CRUMB(PBL_BT_PFS, PBL_BT_PFS_FORMAT));
  cl_assert_equal_i(boot->previous.trail[1].uptime_ms, 31500);
  cl_assert_equal_i(boot->previous.notes[PBL_BT_NOTE_RES_CRC], 0x12345678U);
  cl_assert(boot->previous.notes_set & (1U << PBL_BT_NOTE_RES_CRC));

  // This run's record is new, with the reason the run before set
  cl_assert_equal_i(pbl_boot_trace_current()->boot_count, 2);
  cl_assert_equal_i(pbl_boot_trace_current()->trail_count, 1);
  cl_assert_equal_i(pbl_boot_trace_current()->notes_set, 0);
  cl_assert_equal_i(pbl_boot_trace_current()->reboot_reason[0], 0x11U);
}

void test_boot_trace__ram_lost_keeps_the_register(void) {
  const struct pbl_boot_trace_boot *boot;

  pbl_boot_trace_init();
  pbl_boot_trace_crumb(PBL_BT_RES, PBL_BT_RES_MISSING);

  // Power lost to the RAM only
  memset(test_boot_trace_ram, 0, sizeof(test_boot_trace_ram));
  pbl_boot_trace_init();
  boot = pbl_boot_trace_boot_info();

  cl_assert(boot->bkp_valid);
  cl_assert(!boot->ram_valid);
  cl_assert_equal_i(boot->bkp_crumb, PBL_BOOT_TRACE_CRUMB(PBL_BT_RES, PBL_BT_RES_MISSING));
  cl_assert_equal_i(pbl_boot_trace_current()->boot_count, 2);
}

void test_boot_trace__record_of_an_older_run(void) {
  pbl_boot_trace_init();
  pbl_boot_trace_init();
  // Another image counted a boot without a record in RAM
  s_bkp[31] += 1U;
  pbl_boot_trace_init();

  cl_assert(pbl_boot_trace_boot_info()->ram_valid);
  cl_assert(!pbl_boot_trace_boot_info()->ram_current);
}

void test_boot_trace__trail_wraps(void) {
  const struct pbl_boot_trace_record *prev;

  pbl_boot_trace_init();
  for (uint32_t i = 0U; i < 100U; i++) {
    pbl_boot_trace_crumb(PBL_BT_NOR, PBL_BT_NOR_ERASE | i);
  }
  pbl_boot_trace_init();
  prev = &pbl_boot_trace_boot_info()->previous;

  cl_assert_equal_i(prev->trail_count, 101);
  cl_assert_equal_i(prev->trail[(prev->trail_count - 1U) % PBL_BOOT_TRACE_TRAIL].crumb,
                    PBL_BOOT_TRACE_CRUMB(PBL_BT_NOR, PBL_BT_NOR_ERASE | 99U));
}

void test_boot_trace__first_fault_and_panic_kept(void) {
  const struct pbl_boot_trace_record *prev;
  struct pbl_boot_trace_fault f = {.kind = 5U, .pc = 0x08061234U, .lr = 0x08065678U};

  pbl_boot_trace_init();
  pbl_boot_trace_crumb(PBL_BT_MMC, PBL_BT_MMC_READ_FAILED);
  pbl_boot_trace_panic("flash.c", 641, 0x08070000U, "Flash read failed: -5");
  pbl_boot_trace_fault(&f);
  f.pc = 0xDEADBEEFU;
  pbl_boot_trace_fault(&f);
  pbl_boot_trace_panic("other.c", 1, 0U, "later");

  pbl_boot_trace_init();
  prev = &pbl_boot_trace_boot_info()->previous;
  cl_assert_equal_i(prev->fault.kind, 5);
  cl_assert_equal_i(prev->fault.pc, 0x08061234U);
  cl_assert_equal_i(prev->fault.crumb, PBL_BOOT_TRACE_CRUMB(PBL_BT_MMC, PBL_BT_MMC_READ_FAILED));
  cl_assert(prev->panic.valid);
  cl_assert_equal_s(prev->panic.file, "flash.c");
  cl_assert_equal_i(prev->panic.line, 641);
  cl_assert_equal_s(prev->panic.msg, "Flash read failed: -5");
  cl_assert_equal_i(pbl_boot_trace_current()->fault.kind, 0);
}

void test_boot_trace__area_names(void) {
  cl_assert_equal_s(pbl_boot_trace_area_name(PBL_BT_PFS), "pfs");
  cl_assert_equal_s(pbl_boot_trace_area_name(PBL_BT_LAB + 3U), "lab");
  cl_assert_equal_s(pbl_boot_trace_area_name(0x30U), "?");
}
