/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// Measures what the watch draws from its battery over a window, on the fuel gauge, without
// unplugging it: the charger's input is suspended meanwhile, so that the system runs from the
// battery and nothing charges, and the USB developer access can be held off. Everything is put
// back at the end of the window, at a restart or power-off, and by the charger driver's init.

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/drivers/charger.h>
#include <pbl/drivers/gauge.h>
#include <pbl/kernel/idle.h>
#include <pbl/kernel/mutex.h>
#include <pbl/kernel/reboot.h>
#include <pbl/kernel/time.h>
#include <pbl/kernel/work.h>
#include <pbl/logging/logging.h>
#include <pbl/shell/shell.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#ifdef CONFIG_DEV_USB
#include <pbl/services/dev_usb.h>
#endif

PBL_LOG_MODULE_DECLARE(service_battery, CONFIG_SERVICE_BATTERY_LOG_LEVEL);

// The draw settles after the switch, and so does AvgCurrent, an average over seconds: the samples
// of the start of a window are left out
#define SETTLE_S  30U
#define SAMPLE_MS 2000U
#define MIN_S     60U
#define MAX_S     1800U
// Not on a battery that could run low meanwhile, nor outside the charge temperatures
#define MIN_SOC_CPCT 3000U
#define MIN_TEMP_MC  0
#define MAX_TEMP_MC  45000
#define RESULTS      8U
#define LABEL_LEN    16U

struct pm_result {
  char label[LABEL_LEN];
  uint32_t start_s;
  uint32_t secs;
  uint32_t samples;
  bool usb;
  int ret;
  // Sums over the samples after the settling time, in uA
  int64_t cur_sum;
  int64_t avg_sum;
  // Sum of the squares of the currents, in uA^2, for their spread
  uint64_t cur_sq;
  int32_t cur_min;
  int32_t cur_max;
  // Coulomb counter over the same samples, in counts, and its uAh a count
  int32_t counter_delta;
  uint16_t counter_uah;
  uint32_t counter_secs;
  int32_t mv_start;
  int32_t mv_end;
  // CPU time over the window, per idle state, in ms
  uint32_t total_ms;
  uint32_t idle_ms[PBL_IDLE_STATES_MAX];
};

static PBL_MUTEX_DEFINE(s_lock);
static struct pbl_delayed_work s_work;
static bool s_work_ready;
static bool s_running;
static bool s_suspended;
static bool s_usb_held;
static bool s_gauge_held;
static uint64_t s_start_ms;
static uint64_t s_first_ms;
static uint16_t s_first_counter;
static uint8_t s_first_resets;
static uint64_t s_idle_us[PBL_IDLE_STATES_MAX];
static struct pm_result s_cur;
static struct pm_result s_results[RESULTS];
static uint32_t s_num_results;

static void prv_restore(void) {
  if (s_gauge_held) {
    (void)pbl_gauge_hold_full_rate(GAUGE, false);
    s_gauge_held = false;
  }
  if (s_suspended) {
    int ret = pbl_charger_input_suspend(CHARGER, false);

    if (ret != 0) {
      PBL_LOG_ERR("Charger input restore failed (%d)", ret);
    }
    s_suspended = false;
  }
#ifdef CONFIG_DEV_USB
  if (s_usb_held) {
    dev_usb_hold_off(false);
    s_usb_held = false;
  }
#endif
}

static void prv_finish(int ret) {
  uint64_t now_ms = pbl_uptime_ms();
  struct pm_result *r = &s_cur;

  prv_restore();
  s_running = false;
  r->ret = ret;
  r->secs = (uint32_t)((now_ms - s_start_ms) / 1000U);
  r->total_ms = (uint32_t)(now_ms - s_start_ms);
  for (pbl_idle_state_id_t i = 0U; i < PBL_IDLE_STATES_MAX; i++) {
    r->idle_ms[i] = (uint32_t)((pbl_idle_residency_us(i) - s_idle_us[i]) / 1000U);
  }

  s_results[s_num_results % RESULTS] = *r;
  s_num_results++;

  if (r->samples > 0U) {
    PBL_LOG_INFO("power %s: %" PRIu32 " s, %" PRIu32 " samples, current %" PRId32
                 " uA, average current %" PRId32 " uA, usb %u, ret %d",
                 r->label, r->secs, r->samples, (int32_t)(r->cur_sum / r->samples),
                 (int32_t)(r->avg_sum / r->samples), r->usb, ret);
  } else {
    PBL_LOG_INFO("power %s: no samples, ret %d", r->label, ret);
  }
}

static void prv_sample(struct pbl_work *work) {
  struct pbl_gauge_reading g;
  uint64_t now_ms;
  int ret;

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (!s_running) {
    pbl_mutex_unlock(&s_lock);
    return;
  }

  now_ms = pbl_uptime_ms();
  ret = pbl_gauge_read(GAUGE, &g);
  if (ret != 0) {
    prv_finish(ret);
    pbl_mutex_unlock(&s_lock);
    return;
  }

  // Ends early on a low battery or out of the temperatures
  if (g.soc_cpct < MIN_SOC_CPCT / 2U || g.temp_mc < MIN_TEMP_MC || g.temp_mc > MAX_TEMP_MC) {
    prv_finish(-ERANGE);
    pbl_mutex_unlock(&s_lock);
    return;
  }

  if (now_ms - s_start_ms >= SETTLE_S * 1000U) {
    struct pm_result *r = &s_cur;

    if (r->samples == 0U) {
      s_first_ms = now_ms;
      s_first_counter = g.counter;
      s_first_resets = g.resets;
      r->mv_start = g.avg_voltage_mv;
      r->cur_min = g.current_ua;
      r->cur_max = g.current_ua;
    }
    r->samples++;
    r->cur_sum += g.current_ua;
    r->cur_sq += (uint64_t)((int64_t)g.current_ua * g.current_ua);
    r->avg_sum += g.avg_current_ua;
    r->cur_min = PBL_MIN(r->cur_min, g.current_ua);
    r->cur_max = PBL_MAX(r->cur_max, g.current_ua);
    r->mv_end = g.avg_voltage_mv;
    if (g.resets == s_first_resets) {
      r->counter_delta = (int16_t)(uint16_t)(s_first_counter - g.counter);
      r->counter_uah = g.counter_uah;
      r->counter_secs = (uint32_t)((now_ms - s_first_ms) / 1000U);
    }
  }

  if (now_ms - s_start_ms >= (uint64_t)s_cur.secs * 1000U) {
    prv_finish(0);
  } else {
    pbl_delayed_work_schedule_next(&s_work, pbl_ms_to_ticks(SAMPLE_MS));
  }
  pbl_mutex_unlock(&s_lock);
}

static int prv_start(uint32_t secs, bool usb, const char *label) {
  struct pbl_gauge_reading g;
  int ret;

  if (s_running) {
    return -EBUSY;
  }

  ret = pbl_gauge_read(GAUGE, &g);
  if (ret != 0) {
    return ret;
  }
  if (g.soc_cpct < MIN_SOC_CPCT || g.temp_mc < MIN_TEMP_MC || g.temp_mc > MAX_TEMP_MC) {
    return -ERANGE;
  }

  if (!s_work_ready) {
    pbl_delayed_work_init(&s_work, prv_sample);
    s_work_ready = true;
  }

  s_cur = (struct pm_result){.secs = secs, .usb = usb};
  strncpy(s_cur.label, label, LABEL_LEN - 1U);
  s_cur.start_s = (uint32_t)(pbl_uptime_ms() / 1000U);

  // Plugged in, the system runs from the input: suspended, from the battery the gauge measures.
  // Unplugged there is nothing to suspend.
  ret = pbl_charger_input_suspend(CHARGER, true);
  if (ret == 0) {
    s_suspended = true;
  } else if (ret != -ENOTSUP) {
    return ret;
  }

  // At a low draw the gauge would measure only every few seconds
  s_gauge_held = pbl_gauge_hold_full_rate(GAUGE, true) == 0;

#ifdef CONFIG_DEV_USB
  if (!usb) {
    dev_usb_hold_off(true);
    s_usb_held = true;
  }
#endif

  s_running = true;
  s_start_ms = pbl_uptime_ms();
  for (pbl_idle_state_id_t i = 0U; i < PBL_IDLE_STATES_MAX; i++) {
    s_idle_us[i] = pbl_idle_residency_us(i);
  }
  (void)pbl_delayed_work_reschedule(&pbl_sys_workq, &s_work, PBL_MSEC(SAMPLE_MS));

  return 0;
}

// The input back before a restart or power-off: the stub, which a slot build restarts into, and
// the bootloader leave it as they find it
static void prv_sys_off(enum pbl_sys_off_mode mode, void *user_data) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_running) {
    prv_finish(-ECANCELED);
  }
  pbl_mutex_unlock(&s_lock);
}

PBL_SYS_OFF_HANDLER_DEFINE(power_measure_restart, PBL_SYS_OFF_RESTART_PREPARE,
                           PBL_SYS_OFF_PRIO_DEFAULT, prv_sys_off, nullptr);
PBL_SYS_OFF_HANDLER_DEFINE(power_measure_power_off, PBL_SYS_OFF_POWER_OFF_PREPARE,
                           PBL_SYS_OFF_PRIO_DEFAULT, prv_sys_off, nullptr);

static uint32_t prv_isqrt(uint64_t v) {
  uint64_t r = 0U;

  for (uint64_t bit = 1ULL << 62; bit != 0U; bit >>= 2) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
  }

  return (uint32_t)r;
}

// The standard deviation of the currents sampled, in uA
static uint32_t prv_spread(const struct pm_result *r) {
  const int64_t mean = r->cur_sum / (int64_t)r->samples;
  const int64_t var = (int64_t)(r->cur_sq / r->samples) - mean * mean;

  return var > 0 ? prv_isqrt((uint64_t)var) : 0U;
}

static void prv_print(const struct pbl_shell *sh, const struct pm_result *r) {
  pbl_shell_print(sh, "%s: from %" PRIu32 " s, %" PRIu32 " s, usb %s, %s (%d)", r->label,
                  r->start_s, r->secs, r->usb ? "on" : "held off", r->ret == 0 ? "ok" : "ended",
                  r->ret);
  if (r->samples == 0U) {
    pbl_shell_print(sh, "  no samples");
    return;
  }
  pbl_shell_print(sh,
                  "  current %" PRId32 " uA (min %" PRId32 ", max %" PRId32 ", sd %" PRIu32
                  "), average current %" PRId32 " uA, %" PRIu32 " samples",
                  (int32_t)(r->cur_sum / r->samples), r->cur_min, r->cur_max, prv_spread(r),
                  (int32_t)(r->avg_sum / r->samples), r->samples);
  if (r->counter_secs > 0U) {
    pbl_shell_print(
        sh, "  counter %" PRId32 " x %u uAh in %" PRIu32 " s: %" PRId32 " uA", r->counter_delta,
        r->counter_uah, r->counter_secs,
        (int32_t)(((int64_t)r->counter_delta * r->counter_uah * 3600) / (int64_t)r->counter_secs));
  }
  pbl_shell_print(sh, "  voltage %" PRId32 " to %" PRId32 " mV", r->mv_start, r->mv_end);

  uint32_t idle_ms = 0U;
  for (size_t i = 0U; i < pbl_soc_num_idle_states; i++) {
    idle_ms += r->idle_ms[i];
  }
  pbl_shell_print(
      sh, "  cpu: run %" PRIu32 ".%02" PRIu32 " %%",
      r->total_ms > idle_ms ? (uint32_t)(((uint64_t)(r->total_ms - idle_ms) * 100U) / r->total_ms)
                            : 0U,
      r->total_ms > idle_ms
          ? (uint32_t)((((uint64_t)(r->total_ms - idle_ms) * 10000U) / r->total_ms) % 100U)
          : 0U);
  for (size_t i = 0U; i < pbl_soc_num_idle_states; i++) {
    uint32_t pct100 =
        r->total_ms > 0U ? (uint32_t)(((uint64_t)r->idle_ms[i] * 10000U) / r->total_ms) : 0U;

    pbl_shell_print(sh, "       %s %" PRIu32 ".%02" PRIu32 " %%", pbl_soc_idle_states[i].name,
                    pct100 / 100U, pct100 % 100U);
  }
}

static int prv_cmd_power(const struct pbl_shell *sh, size_t argc, char **argv) {
  int ret = 0;

  pbl_mutex_lock(&s_lock, PBL_FOREVER);

  if (argc >= 3 && strcmp(argv[1], "measure") == 0) {
    uint32_t secs = (uint32_t)strtoul(argv[2], nullptr, 10);
    bool usb = false;
    const char *label = "-";

    for (size_t i = 3U; i < argc; i++) {
      if (strcmp(argv[i], "usb") == 0) {
        usb = true;
      } else {
        label = argv[i];
      }
    }

    if (secs < MIN_S || secs > MAX_S) {
      pbl_shell_error(sh, "from %u to %u s", MIN_S, MAX_S);
      ret = -EINVAL;
    } else {
      ret = prv_start(secs, usb, label);
      if (ret == 0) {
        pbl_shell_print(sh, "measuring %s for %" PRIu32 " s%s: 'power' shows the result", label,
                        secs, usb ? "" : ", the USB off meanwhile");
      } else {
        pbl_shell_error(sh, "cannot start (%d)%s", ret,
                        ret == -ERANGE ? ": battery low or temperature out of range" : "");
      }
    }
  } else if (argc == 2 && strcmp(argv[1], "abort") == 0) {
    if (s_running) {
      prv_finish(-ECANCELED);
    }
  } else if (argc == 1) {
    if (s_running) {
      pbl_shell_print(sh, "measuring %s: %" PRIu32 " of %" PRIu32 " s", s_cur.label,
                      (uint32_t)((pbl_uptime_ms() - s_start_ms) / 1000U), s_cur.secs);
    }
    for (uint32_t n = s_num_results > RESULTS ? s_num_results - RESULTS : 0U; n < s_num_results;
         n++) {
      prv_print(sh, &s_results[n % RESULTS]);
    }
  } else {
    pbl_shell_error(sh, "expected nothing, measure SECONDS [usb] [LABEL], or abort");
    ret = -EINVAL;
  }

  pbl_mutex_unlock(&s_lock);

  return ret;
}

PBL_SHELL_CMD_ARG_REGISTER(power, nullptr,
                           "Battery draw on the gauge: measure SECONDS [usb] [LABEL] | abort",
                           prv_cmd_power, 1, 4);
