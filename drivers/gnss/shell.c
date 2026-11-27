/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <pbl/drivers/gnss.h>
#include <pbl/shell/shell.h>
#ifdef CONFIG_GNSS_SIM
#include <pbl/drivers/gnss/sim.h>
#endif

#ifdef CONFIG_GNSS_SIM
// "gnss signal 0|1" takes the signal away, "gnss hold 0|1" stands still where the route is.
static int prv_cmd_sim(const struct pbl_shell *sh, const char *what, bool on) {
  struct pbl_gnss_fix fix;

  if (strcmp(what, "signal") == 0) {
    pbl_gnss_sim_set_signal(SIM_GNSS, on);
    return 0;
  }
  if (strcmp(what, "hold") == 0) {
    if (!on) {
      pbl_gnss_sim_set_fix(SIM_GNSS, nullptr);
      return 0;
    }
    if (pbl_gnss_peek(SIM_GNSS, &fix) != 0) {
      pbl_shell_error(sh, "no epoch yet");
      return -ENODATA;
    }
    fix.speed_mm_s = 0;
    pbl_gnss_sim_set_fix(SIM_GNSS, &fix);
    return 0;
  }

  return -EINVAL;
}
#endif

static int prv_cmd_gnss(const struct pbl_shell *sh, size_t argc, char **argv) {
  struct pbl_gnss_fix fix;
  int ret;

  if (argc >= 2 && strcmp(argv[1], "start") == 0) {
    return pbl_gnss_start(GNSS, 1000);
  }
  if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
    return pbl_gnss_stop(GNSS);
  }

#ifdef CONFIG_GNSS_SIM
  if (argc >= 3) {
    return prv_cmd_sim(sh, argv[1], strcmp(argv[2], "0") != 0);
  }
#endif

  ret = pbl_gnss_peek(GNSS, &fix);
  if (ret != 0) {
    pbl_shell_error(sh, "no epoch (%d)%s", ret, pbl_gnss_is_running(GNSS) ? "" : ", stopped");
    return ret;
  }

  pbl_shell_print(sh,
                  "q %u sats %u lat %" PRId32 " lon %" PRId32 " alt %" PRId32 " cm speed %" PRIu32
                  " mm/s course %u hdop %u",
                  fix.quality, fix.sats_used, fix.lat_e7, fix.lon_e7, fix.alt_cm, fix.speed_mm_s,
                  fix.course_cdeg, fix.hdop_x10);

  return 0;
}

PBL_SHELL_CMD_REGISTER(gnss, nullptr, "GNSS receiver: gnss [start|stop|signal 0|1|hold 0|1]",
                       prv_cmd_gnss);
