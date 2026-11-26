/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <inttypes.h>

#include <pbl/drivers/gauge.h>
#include <pbl/shell/shell.h>

static int prv_cmd_gauge(const struct pbl_shell *sh, size_t argc, char **argv) {
  struct pbl_gauge_reading r;
  int ret;

  ret = pbl_gauge_read(GAUGE, &r);
  if (ret != 0) {
    pbl_shell_error(sh, "read failed (%d)", ret);
    return ret;
  }

  pbl_shell_print(sh, "soc: %u.%02u %%", r.soc_cpct / 100U, r.soc_cpct % 100U);
  pbl_shell_print(sh, "voltage: %" PRId32 " mV (average %" PRId32 " mV)", r.voltage_mv,
                  r.avg_voltage_mv);
  pbl_shell_print(sh, "current: %" PRId32 " uA", r.current_ua);
  pbl_shell_print(sh, "temperature: %" PRId32 " mC", r.temp_mc);

  return 0;
}

PBL_SHELL_CMD_REGISTER(gauge, nullptr, "Read the fuel gauge", prv_cmd_gauge);
