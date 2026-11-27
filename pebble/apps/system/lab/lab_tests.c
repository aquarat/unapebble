/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// Tests of the trace itself and of the ways a run ends: each resets the watch, or powers it off,
// and the next boot's report shows what it kept.

#include "lab.h"

#include <stdint.h>

#include <pbl/kernel/assert.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/reboot.h>
#include <pbl/util/size.h>

// No memory there: a precise bus fault at this address
#define NO_MEMORY 0x60000000U

static int prv_fault(void) {
  lab_printf("reading 0x%08x", NO_MEMORY);
  (void)*(volatile uint32_t *)NO_MEMORY;
  lab_printf("NO FAULT");

  return -1;
}

static int prv_assert(void) {
  PBL_ASSERT(false, "Lab assert test");

  return -1;
}

static int prv_watchdog(void) {
  // Nothing runs, the watchdog thread included: the hardware watchdog resets the watch
  pbl_irq_lock();
  for (;;) {
  }

  return -1;
}

static int prv_reset(void) {
  pbl_reboot();
}

static int prv_power_off(void) {
  pbl_power_off();
}

static const struct lab_step s_fault_steps[] = {{"bus fault", prv_fault}};
static const struct lab_step s_assert_steps[] = {{"assert", prv_assert}};
static const struct lab_step s_watchdog_steps[] = {{"spin, interrupts off", prv_watchdog}};
static const struct lab_step s_reset_steps[] = {{"reboot", prv_reset}};
static const struct lab_step s_power_off_steps[] = {{"power off", prv_power_off}};

const struct lab_experiment lab_exp_test_fault = {
  .title = "Test: fault",
  .subtitle = "Bus fault, recorded",
  .steps = s_fault_steps,
  .num_steps = PBL_ARRAY_SIZE(s_fault_steps),
  .confirm =
      "Reads memory that is not there: the watch resets, and the next boot shows the "
      "fault's PC and BFAR 0x60000000.",
};

const struct lab_experiment lab_exp_test_assert = {
  .title = "Test: assert",
  .subtitle = "Failed assert, recorded",
  .steps = s_assert_steps,
  .num_steps = PBL_ARRAY_SIZE(s_assert_steps),
  .confirm =
      "Fails an assert: the watch resets, and the next boot shows lab_tests.c and its "
      "line.",
};

const struct lab_experiment lab_exp_test_watchdog = {
  .title = "Test: watchdog",
  .subtitle = "Hang until the IWDG resets",
  .steps = s_watchdog_steps,
  .num_steps = PBL_ARRAY_SIZE(s_watchdog_steps),
  .confirm =
      "Hangs with interrupts off: the screen freezes and the hardware watchdog resets "
      "the watch within 30 s. The next boot shows IWDG.",
};

const struct lab_experiment lab_exp_test_reset = {
  .title = "Test: reset",
  .subtitle = "Software reset",
  .steps = s_reset_steps,
  .num_steps = PBL_ARRAY_SIZE(s_reset_steps),
  .confirm =
      "Restarts the watch. The next boot shows a software reset and whether the RAM "
      "record survived it.",
};

const struct lab_experiment lab_exp_test_power_off = {
  .title = "Test: power off",
  .subtitle = "As B does without resources",
  .steps = s_power_off_steps,
  .num_steps = PBL_ARRAY_SIZE(s_power_off_steps),
  .confirm =
      "Powers off through the bootloader, as an eMMC build does when it finds no "
      "resources. Note whether the watch stays off, then hold the power button to "
      "turn it on.",
};
