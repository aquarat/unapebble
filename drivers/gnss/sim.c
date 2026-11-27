/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "sim_track.h"

#include <errno.h>

#include <pbl/drivers/gnss.h>
#include <pbl/drivers/gnss/sim.h>
#include <pbl/kernel/time.h>
#include <pbl/kernel/work.h>

static struct gnss_sim_track s_track;
static struct pbl_gnss_fix s_override;
static bool s_has_override;
static bool s_signal;
static uint32_t s_searching_s;
static uint32_t s_interval_ms;
static struct pbl_delayed_work s_timer;

static void prv_epoch(struct pbl_gnss_fix *fix) {
  *fix = (struct pbl_gnss_fix){.utc_ms = pbl_realtime_ms()};

  /* The route goes on while searching, as the runner would. */
  gnss_sim_track_step(&s_track, fix);

  if (s_searching_s > 0 || !s_signal) {
    const uint64_t utc_ms = s_searching_s > 0 ? 0 : fix->utc_ms;
    const uint8_t sats =
        s_searching_s > 0 ? (uint8_t)(CONFIG_GNSS_SIM_TIME_TO_FIX - s_searching_s) : 0;

    *fix = (struct pbl_gnss_fix){.utc_ms = utc_ms, .sats_used = sats};
    if (s_searching_s > 0) {
      s_searching_s--;
    }
    return;
  }

  fix->sats_used = (uint8_t)(14 + (fix->hdop_x10 & 3));
  fix->quality = PBL_GNSS_FIX_3D;
}

static void prv_timer_fn(struct pbl_work *work) {
  const struct pbl_gnss *gnss = SIM_GNSS;
  struct pbl_gnss_fix fix;

  pbl_gnss_lock(gnss);

  if (s_interval_ms != 0U) {
    if (s_has_override) {
      fix = s_override;
    } else {
      prv_epoch(&fix);
    }
    pbl_gnss_report_fix(gnss, &fix);

    if (!pbl_delayed_work_is_pending(&s_timer)) {
      pbl_delayed_work_schedule_next(&s_timer, pbl_ms_to_ticks(s_interval_ms));
    }
  }

  pbl_gnss_unlock(gnss);
}

void pbl_gnss_sim_set_signal(const struct pbl_gnss *gnss, bool available) {
  pbl_gnss_lock(gnss);
  s_signal = available;
  pbl_gnss_unlock(gnss);
}

void pbl_gnss_sim_set_fix(const struct pbl_gnss *gnss, const struct pbl_gnss_fix *fix) {
  pbl_gnss_lock(gnss);
  s_has_override = fix != nullptr;
  if (fix != nullptr) {
    s_override = *fix;
  }
  pbl_gnss_unlock(gnss);
}

void pbl_gnss_sim_reset(const struct pbl_gnss *gnss) {
  pbl_gnss_lock(gnss);
  gnss_sim_track_reset(&s_track);
  s_searching_s = CONFIG_GNSS_SIM_TIME_TO_FIX;
  pbl_gnss_unlock(gnss);
}

static int prv_init(const struct pbl_gnss *gnss) {
  gnss_sim_track_reset(&s_track);
  s_signal = true;
  s_has_override = false;
  s_interval_ms = 0U;
  pbl_delayed_work_init(&s_timer, prv_timer_fn);

  return 0;
}

static int prv_start(const struct pbl_gnss *gnss, uint32_t interval_ms) {
  if (s_interval_ms == 0U) {
    s_searching_s = CONFIG_GNSS_SIM_TIME_TO_FIX;
  }
  s_interval_ms = interval_ms;
  pbl_delayed_work_reschedule(&pbl_sys_workq, &s_timer, PBL_MSEC(interval_ms));

  return 0;
}

static int prv_stop(const struct pbl_gnss *gnss) {
  s_interval_ms = 0U;
  pbl_delayed_work_cancel(&s_timer);

  return 0;
}

static const struct pbl_gnss_ops s_ops = {
  .init = prv_init,
  .start = prv_start,
  .stop = prv_stop,
};

PBL_GNSS_STATE_DEFINE(pbl_gnss_sim);
const struct pbl_gnss pbl_gnss_sim = PBL_GNSS_INIT(pbl_gnss_sim, "gnss", nullptr, &s_ops);
PBL_DEVICE_REGISTER(pbl_gnss_sim, &pbl_gnss_sim.dev);
