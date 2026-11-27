/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#include <pbl/drivers/gnss.h>
#include <pbl/shell/shell.h>
#ifdef CONFIG_GNSS_SIM
#include <pbl/drivers/gnss/sim.h>
#endif
#ifdef CONFIG_GNSS_AG3335
#include <pbl/drivers/gnss/ag3335.h>
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

#ifdef CONFIG_GNSS_AG3335
// "gnss pair PAIR066,1,0,0,0,0,0": a command to the running receiver, and its answer
static int prv_cmd_pair(const struct pbl_shell *sh, const char *cmd) {
  char reply[96];
  int ret = pbl_gnss_ag3335_command(GNSS, cmd, reply, sizeof(reply));

  if (ret < 0) {
    pbl_shell_error(sh, "%s: %d", cmd, ret);
    return ret;
  }

  pbl_shell_print(sh, "%s: result %d%s%s", cmd, ret, reply[0] != '\0' ? ", " : "", reply);

  return 0;
}
#endif

static const char *const s_quality[] = {"none", "2D", "3D", "DGNSS"};

static void prv_print_fix(const struct pbl_shell *sh, const struct pbl_gnss_fix *fix) {
  char when[24] = "unknown";

  if (fix->utc_ms != 0U) {
    time_t t = (time_t)(fix->utc_ms / 1000U);
    struct tm tm;

    (void)gmtime_r(&t, &tm);
    (void)strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%SZ", &tm);
  }

  pbl_shell_print(sh, "fix %s, %u used, %u in view, time %s",
                  fix->quality < 4U ? s_quality[fix->quality] : "?", fix->sats_used,
                  fix->sats_in_view, when);
  if (fix->quality != PBL_GNSS_FIX_NONE) {
    pbl_shell_print(sh,
                    "lat %" PRId32 " lon %" PRId32 " (1e-7 deg) alt %" PRId32 " cm speed %" PRIu32
                    " mm/s course %u cdeg hdop %u.%u",
                    fix->lat_e7, fix->lon_e7, fix->alt_cm, fix->speed_mm_s, fix->course_cdeg,
                    fix->hdop_x10 / 10U, fix->hdop_x10 % 10U);
  }
}

static int prv_cmd_status(const struct pbl_shell *sh) {
  struct pbl_gnss_stats st;
  struct pbl_gnss_fix fix;

  pbl_shell_print(sh, "receiver %s", pbl_gnss_is_running(GNSS) ? "started" : "stopped");

  if (pbl_gnss_get_stats(GNSS, &st) == 0) {
    pbl_shell_print(sh, "state %s, firmware %s", st.state,
                    st.version != nullptr ? st.version : "unknown");
    pbl_shell_print(
        sh,
        "rx %" PRIu32 " bytes, sentences GGA %" PRIu32 " RMC %" PRIu32 " GSA %" PRIu32
        " GSV %" PRIu32 " PAIR %" PRIu32 " other %" PRIu32 ", bad %" PRIu32 ", overruns %" PRIu32,
        st.rx_bytes, st.sentences[PBL_GNSS_SENTENCE_GGA], st.sentences[PBL_GNSS_SENTENCE_RMC],
        st.sentences[PBL_GNSS_SENTENCE_GSA], st.sentences[PBL_GNSS_SENTENCE_GSV],
        st.sentences[PBL_GNSS_SENTENCE_PROPRIETARY], st.sentences[PBL_GNSS_SENTENCE_OTHER], st.bad,
        st.overruns);
    pbl_shell_print(sh, "epochs %" PRIu32 ", %u tracked, best %u dB-Hz, first fix %" PRIu32 " ms",
                    st.epochs, st.sats_tracked, st.best_cn0, st.time_to_fix_ms);
  }

  if (pbl_gnss_peek(GNSS, &fix) != 0) {
    pbl_shell_print(sh, "no epoch since started");
    return 0;
  }

  prv_print_fix(sh, &fix);

  return 0;
}

static int prv_cmd_gnss(const struct pbl_shell *sh, size_t argc, char **argv) {
  struct pbl_gnss_fix fix;
  int ret;

  if (argc >= 2 && (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "on") == 0)) {
    return pbl_gnss_start(GNSS, 1000);
  }
  if (argc >= 2 && (strcmp(argv[1], "stop") == 0 || strcmp(argv[1], "off") == 0)) {
    return pbl_gnss_stop(GNSS);
  }
  if (argc >= 2 && strcmp(argv[1], "status") == 0) {
    return prv_cmd_status(sh);
  }

#ifdef CONFIG_GNSS_AG3335
  if (argc >= 3 && strcmp(argv[1], "pair") == 0) {
    return prv_cmd_pair(sh, argv[2]);
  }
#endif

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

PBL_SHELL_CMD_REGISTER(gnss, nullptr,
                       "GNSS receiver: gnss [on|off|status|pair PAIRnnn,...|signal 0|1|hold 0|1]",
                       prv_cmd_gnss);
