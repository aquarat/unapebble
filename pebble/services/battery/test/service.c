/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The battery capacity test: samples on the battery service's polls, saves its state at each
// change of phase and logs the samples, so that it carries on across the low battery power-off.

#include "core.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/drivers/charger.h>
#include <pbl/fs/fs.h>
#include <pbl/kernel/compiler.h>
#include <pbl/kernel/mutex.h>
#include <pbl/kernel/reboot.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/services/battery/battery_state.h>
#include <pbl/services/battery/battery_test.h>
#include <pbl/util/math.h>

PBL_LOG_MODULE_DECLARE(service_battery, CONFIG_SERVICE_BATTERY_LOG_LEVEL);

#define STATE_PATH "/battest"
// Two logs: decimating copies one into the other, PFS cannot rename
#define LOG0_PATH "/battest.0"
#define LOG1_PATH "/battest.1"
// 8192 records of 36 bytes: 5.7 days a minute apart, then decimated
#define LOG_RECORDS 8192U
#define LOG_SIZE    (LOG_RECORDS * sizeof(struct bt_record))
#define MAGIC       (0x42540000U | (uint32_t)sizeof(struct bt_state))
// The battery service polls every 30 s plugged in, 60 s otherwise
#define SAMPLE_MS (55U * 1000U)
// Records copied at once when decimating, and printed at once
#define CHUNK 8U
// The two capacities UNA's sources give the cell
#define UNA_MAH_1 200
#define UNA_MAH_2 280

static_assert(sizeof(struct bt_record) == 36, "log format");
static_assert((int)BATTERY_TEST_DONE == (int)BT_DONE, "phases");

static PBL_MUTEX_DEFINE(s_lock);
static struct bt_state s_st;
static struct bt_run s_run;
static bool s_loaded;
static uint32_t s_count;
static uint32_t s_samples;
static uint64_t s_last_ms;
static uint8_t s_last_flags = 0xFFU;
static volatile bool s_heavy_running;
static struct pbl_gauge_reading s_last;

static const char *prv_log(uint8_t shift) {
  return ((shift & 1U) != 0U) ? LOG1_PATH : LOG0_PATH;
}

static bool prv_active(void) {
  return (s_st.phase != BT_IDLE) && (s_st.phase != BT_DONE);
}

static bool prv_read_at(struct pbl_fs_file *f, uint32_t idx, struct bt_record *rec, size_t n) {
  const size_t len = n * sizeof(*rec);

  return (pbl_fs_seek(f, (off_t)(idx * sizeof(*rec)), PBL_FS_SEEK_SET) >= 0) &&
         (pbl_fs_read(f, rec, len) == (ssize_t)len);
}

// Records are written in order into erased flash: the first one that reads erased ends the log
static uint32_t prv_log_count(void) {
  struct pbl_fs_file f;
  struct bt_record rec;
  uint32_t lo = 0U;
  uint32_t hi = LOG_RECORDS;

  if (pbl_fs_open(&f, prv_log(s_st.log_shift), PBL_FS_O_READ) < 0) {
    return 0U;
  }
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2U;

    if (!prv_read_at(&f, mid, &rec, 1U) || (rec.time == UINT32_MAX)) {
      hi = mid;
    } else {
      lo = mid + 1U;
    }
  }
  if ((lo > 0U) && prv_read_at(&f, lo - 1U, &rec, 1U)) {
    bt_run_restore(&s_run, &rec);
  }
  pbl_fs_close(&f);

  return lo;
}

static void prv_load(void) {
  struct pbl_fs_file f;

  if (s_loaded) {
    return;
  }
  s_loaded = true;

  if (pbl_fs_open(&f, STATE_PATH, PBL_FS_O_READ) < 0) {
    return;
  }
  if ((pbl_fs_read(&f, &s_st, sizeof(s_st)) != (ssize_t)sizeof(s_st)) || (s_st.magic != MAGIC)) {
    s_st = (struct bt_state){0};
  }
  pbl_fs_close(&f);

  if (prv_active()) {
    s_count = prv_log_count();
    PBL_LOG_INFO("Battery test resumes in phase %u, %" PRIu32 " records", s_st.phase, s_count);
  }
}

static int prv_create(const char *path, uint32_t flags, size_t size, const void *data) {
  struct pbl_fs_file f;
  int ret;

  ret = pbl_fs_open(&f, path, flags);
  if (ret < 0) {
    return ret;
  }
  ret = pbl_fs_truncate(&f, (off_t)size);
  if ((ret == 0) && (data != NULL) && (pbl_fs_write(&f, data, size) != (ssize_t)size)) {
    ret = -EIO;
  }
  pbl_fs_close(&f);

  return ret;
}

static void prv_save(void) {
  const int ret = prv_create(STATE_PATH, PBL_FS_O_REPLACE | PBL_FS_O_CREATE, sizeof(s_st), &s_st);

  if (ret < 0) {
    PBL_LOG_ERR("Battery test state not saved (%d)", ret);
  }
}

// A full log keeps every other record, and every other sample from then on
static bool prv_decimate(void) {
  const char *from = prv_log(s_st.log_shift);
  const char *to = prv_log(s_st.log_shift + 1U);
  struct pbl_fs_file in;
  struct pbl_fs_file out;
  struct bt_record buf[CHUNK];
  uint32_t kept = 0U;
  bool ok = true;

  pbl_fs_unlink(to);
  if (prv_create(to, PBL_FS_O_WRITE | PBL_FS_O_CREATE, LOG_SIZE, NULL) < 0) {
    return false;
  }
  if (pbl_fs_open(&in, from, PBL_FS_O_READ) < 0) {
    return false;
  }
  if (pbl_fs_open(&out, to, PBL_FS_O_WRITE) < 0) {
    pbl_fs_close(&in);
    return false;
  }
  for (uint32_t i = 0U; ok && (i < s_count); i += CHUNK) {
    ok = prv_read_at(&in, i, buf, CHUNK);
    for (uint32_t j = 0U; ok && (j < CHUNK); j += 2U) {
      ok = pbl_fs_write(&out, &buf[j], sizeof(buf[j])) == (ssize_t)sizeof(buf[j]);
      kept++;
    }
  }
  pbl_fs_close(&in);
  pbl_fs_close(&out);
  if (!ok) {
    return false;
  }

  // The saved state names the log: the old one goes once the new one is
  s_count = kept;
  s_st.log_shift++;
  prv_save();
  pbl_fs_unlink(from);

  return true;
}

static void prv_append(const struct bt_record *rec) {
  struct pbl_fs_file f;

  if ((s_count >= LOG_RECORDS) && !prv_decimate()) {
    PBL_LOG_ERR("Battery test log full");
    return;
  }
  if (pbl_fs_open(&f, prv_log(s_st.log_shift), PBL_FS_O_WRITE) < 0) {
    return;
  }
  if ((pbl_fs_seek(&f, (off_t)(s_count * sizeof(*rec)), PBL_FS_SEEK_SET) >= 0) &&
      (pbl_fs_write(&f, rec, sizeof(*rec)) == (ssize_t)sizeof(*rec))) {
    s_count++;
  }
  pbl_fs_close(&f);
}

static uint8_t prv_flags(bool plugged, uint8_t pct) {
  enum pbl_charger_status cs = PBL_CHARGER_STATUS_UNKNOWN;
  uint8_t flags = 0U;

  if (plugged) {
    flags |= BT_S_PLUGGED;
    if ((pbl_charger_get_status(CHARGER, &cs) == 0) && (cs == PBL_CHARGER_STATUS_FULL)) {
      flags |= BT_S_FULL;
    }
  } else if (pct == 0U) {
    flags |= BT_S_EMPTY;
  }
  if (s_heavy_running) {
    flags |= BT_S_HEAVY;
  }

  return flags;
}

// Called with the lock held, the state loaded
static void prv_take(const struct pbl_gauge_reading *r, uint8_t flags, bool force) {
  const uint32_t mask = (1UL << s_st.log_shift) - 1U;
  struct bt_sample smp = {.time = (uint32_t)pbl_realtime(), .flags = flags, .r = *r};
  struct bt_record rec;
  unsigned int ret;

  s_last = *r;
  s_last_ms = pbl_uptime_ms();
  force = force || (flags != s_last_flags);
  s_last_flags = flags;

  ret = bt_step(&s_st, &s_run, &smp, &rec);
  if ((ret & BT_STEP_SAVE) != 0U) {
    PBL_LOG_INFO("Battery test phase %u", s_st.phase);
    prv_save();
  }
  if (((ret & BT_STEP_LOG) != 0U) &&
      (((ret & BT_STEP_SAVE) != 0U) || force || ((s_samples & mask) == 0U))) {
    prv_append(&rec);
  }
  s_samples++;
}

void battery_test_sample(const struct pbl_gauge_reading *r, bool plugged, uint8_t pct) {
  uint8_t flags;

  if (r == NULL) {
    return;
  }

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_load();
  s_last = *r;
  if (prv_active()) {
    flags = prv_flags(plugged, pct);
    if (!s_run.valid || (flags != s_last_flags) || (pbl_uptime_ms() - s_last_ms >= SAMPLE_MS)) {
      prv_take(r, flags, false);
    }
  }
  pbl_mutex_unlock(&s_lock);
}

// The last sample before a power-off or a restart, the low battery one among them
static void prv_sys_off(enum pbl_sys_off_mode mode, void *user_data) {
  const BatteryChargeState c = battery_get_charge_state();
  struct pbl_gauge_reading r;

  // Never hold up a power-off for long
  if ((pbl_gauge_read(GAUGE, &r) != 0) || (pbl_mutex_lock(&s_lock, PBL_MSEC(1000)) != 0)) {
    return;
  }
  if (s_loaded && prv_active()) {
    prv_take(&r, prv_flags(c.is_plugged, c.charge_percent), true);
  }
  pbl_mutex_unlock(&s_lock);
}

PBL_SYS_OFF_HANDLER_DEFINE(battery_test_power_off, PBL_SYS_OFF_POWER_OFF_PREPARE,
                           PBL_SYS_OFF_PRIO_HIGH, prv_sys_off, nullptr);
PBL_SYS_OFF_HANDLER_DEFINE(battery_test_restart, PBL_SYS_OFF_RESTART_PREPARE, PBL_SYS_OFF_PRIO_HIGH,
                           prv_sys_off, nullptr);

void battery_test_start(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_load();
  bt_start(&s_st, &s_run, MAGIC, (s_st.flags & BT_F_HEAVY) != 0U);
  pbl_fs_unlink(LOG0_PATH);
  pbl_fs_unlink(LOG1_PATH);
  if (prv_create(LOG0_PATH, PBL_FS_O_WRITE | PBL_FS_O_CREATE, LOG_SIZE, NULL) < 0) {
    PBL_LOG_ERR("Battery test log not created");
  }
  s_count = 0U;
  s_samples = 0U;
  s_last_flags = 0xFFU;
  prv_save();
  pbl_mutex_unlock(&s_lock);

  PBL_LOG_INFO("Battery test started");
  battery_state_force_update();
}

void battery_test_cancel(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_st = (struct bt_state){0};
  s_run = (struct bt_run){0};
  s_loaded = true;
  s_count = 0U;
  pbl_fs_unlink(STATE_PATH);
  pbl_fs_unlink(LOG0_PATH);
  pbl_fs_unlink(LOG1_PATH);
  pbl_mutex_unlock(&s_lock);
}

enum battery_test_phase battery_test_get_phase(void) {
  enum battery_test_phase phase;

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_load();
  phase = (enum battery_test_phase)s_st.phase;
  pbl_mutex_unlock(&s_lock);

  return phase;
}

void battery_test_set_heavy(bool heavy) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_load();
  s_st.flags = (uint8_t)((s_st.flags & ~BT_F_HEAVY) | (heavy ? BT_F_HEAVY : 0U));
  if (s_st.phase != BT_IDLE) {
    prv_save();
  }
  pbl_mutex_unlock(&s_lock);
}

bool battery_test_is_heavy(void) {
  return (s_st.flags & BT_F_HEAVY) != 0U;
}

void battery_test_set_heavy_running(bool running) {
  s_heavy_running = running;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Text

struct prv_text {
  char *p;
  size_t left;
};

PBL_FORMAT_PRINTF(2, 3)
static void prv_cat(struct prv_text *t, const char *fmt, ...) {
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(t->p, t->left, fmt, ap);
  va_end(ap);
  if ((n > 0) && ((size_t)n < t->left)) {
    t->p += n;
    t->left -= (size_t)n;
  } else {
    t->left = (t->left > 0U) ? 1U : 0U;
  }
}

// v / div with one decimal, rounded, into b
static const char *prv_d1(char b[12], int32_t v, int32_t div) {
  const uint32_t a = (uint32_t)abs(v);
  const uint32_t tenths = (a + (uint32_t)div / 20U) / ((uint32_t)div / 10U);

  snprintf(b, 12, "%s%" PRIu32 ".%" PRIu32, (v < 0) ? "-" : "", tenths / 10U, tenths % 10U);
  return b;
}

static void prv_span(struct prv_text *t, const char *what, const struct bt_span *sp) {
  const uint32_t min = (sp->end > sp->start) ? (sp->end - sp->start) / 60U : 0U;
  char a[12];
  char b[12];
  char c[12];

  prv_cat(t, "%s %s mAh (sum %s)\n%" PRIu32 "h%02" PRIu32 "m at %s mA, %d..%d C\n", what,
          prv_d1(a, sp->uah, 1000), prv_d1(b, sp->sw_uah, 1000), min / 60U, min % 60U,
          prv_d1(c, bt_avg_ua(sp->uah, sp->start, sp->end), 1000), sp->tmin_c, sp->tmax_c);
}

static void prv_results(struct prv_text *t) {
  const struct bt_full *f0 = &s_st.full[0];
  const struct bt_full *f = &s_st.full[1];
  char a[12];
  char b[12];
  char c[12];
  char d[12];

  prv_cat(t, "Usable charge from full to the 3.35 V power-off\n");
  prv_span(t, "Out", &s_st.dis);
  prv_cat(t, "%" PRId32 "%% of %d, %" PRId32 "%% of %d mAh\n%s load\n",
          s_st.dis.uah / (UNA_MAH_1 * 10), UNA_MAH_1, s_st.dis.uah / (UNA_MAH_2 * 10), UNA_MAH_2,
          (s_st.loads == BT_LOAD_HEAVY)    ? "Heavy"
          : (s_st.loads == BT_LOAD_NORMAL) ? "Normal"
                                           : "Mixed");
  prv_span(t, "In", &s_st.rec);
  // What the gauge learned, at the end and at the start
  prv_cat(t, "FullCapRep %s (%s) mAh\nFullCapNom %s (%s) mAh\n",
          prv_d1(a, (int32_t)f->full_uah, 1000), prv_d1(b, (int32_t)f0->full_uah, 1000),
          prv_d1(c, (int32_t)f->nominal_uah, 1000), prv_d1(d, (int32_t)f0->nominal_uah, 1000));
  prv_cat(t, "Design %" PRIu32 " mAh, %u.%02u cycles\n", f->design_uah / 1000U,
          f->cycles_pct / 100U, f->cycles_pct % 100U);
  if ((s_st.flags & BT_F_COMPLETE) == 0U) {
    prv_cat(t, "Plugged in before empty!\n");
  }
  if ((s_st.flags & BT_F_LOST) != 0U) {
    prv_cat(t, "Gauge reset: partly summed\n");
  }
}

void battery_test_describe(char *buf, size_t len) {
  static const char *const s_phases[] = {
    [BT_CHARGE] = "1/4 Charge\nPlug in, wait for full",
    [BT_FULL] = "2/4 Full\nUnplug to discharge",
    [BT_DISCHARGE] = "3/4 Discharge\nRuns until the watch powers off; then plug in, power on",
    [BT_RECHARGE] = "4/4 Recharge\nKeep plugged in until full",
  };
  struct prv_text t = {buf, len};
  const struct pbl_gauge_reading *r = &s_last;
  const uint32_t now = (uint32_t)pbl_realtime();
  char a[12];
  char b[12];

  if (len > 0U) {
    buf[0] = '\0';
  }

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_load();
  switch (s_st.phase) {
    case BT_IDLE:
      prv_cat(&t,
              "Battery test\nCharges to full, runs the battery down to the power-off, "
              "charges again, and measures the charge.\nSelect: start");
      break;
    case BT_DONE:
      prv_results(&t);
      prv_cat(&t, "Hold Select: new test");
      break;
    default:
      prv_cat(&t, "%s\n%s load (Select)\n%" PRId32 ".%02" PRId32 " V, %s mA %s, %u%%\n",
              s_phases[s_st.phase], battery_test_is_heavy() ? "Heavy" : "Normal",
              r->voltage_mv / 1000, (r->voltage_mv % 1000) / 10,
              prv_d1(a, abs(r->current_ua), 1000), (r->current_ua < 0) ? "in" : "out",
              r->soc_cpct / 100U);
      if (s_st.phase == BT_DISCHARGE) {
        prv_cat(&t, "Out %s mAh, %" PRIu32 " min\n", prv_d1(b, s_run.out_uah, 1000),
                (now - s_st.dis.start) / 60U);
      } else if (s_st.phase == BT_RECHARGE) {
        prv_cat(&t, "In %s mAh, %" PRIu32 " min\n", prv_d1(b, s_st.dis.uah - s_run.out_uah, 1000),
                (now - s_st.rec.start) / 60U);
      }
      prv_cat(&t, "Hold Select: cancel");
      break;
  }
  pbl_mutex_unlock(&s_lock);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Shell

#ifdef CONFIG_SHELL
#include <pbl/shell/shell.h>

static int prv_cmd_status(const struct pbl_shell *sh, size_t argc, char **argv) {
  char buf[640];
  char *line = buf;

  battery_test_describe(buf, sizeof(buf));
  while (line != NULL) {
    char *nl = strchr(line, '\n');

    if (nl != NULL) {
      *nl++ = '\0';
    }
    pbl_shell_print(sh, "%s", line);
    line = nl;
  }
  pbl_shell_print(sh, "%" PRIu32 " records, every %u samples, %" PRId32 " uAh bridged", s_count,
                  1U << s_st.log_shift, s_st.gap_uah);

  return 0;
}

static int prv_cmd_log(const struct pbl_shell *sh, size_t argc, char **argv) {
  struct bt_record buf[CHUNK];
  struct pbl_fs_file f;

  pbl_shell_print(sh,
                  "time,v_mv,avg_v_mv,i_ua,avg_i_ua,soc_cpct,repcap_uah,fullcaprep_uah,qh,"
                  "temp_mc,flags,phase,loads,out_uah,sum_uah");
  for (uint32_t i = 0U;; i += CHUNK) {
    size_t n;
    bool ok;

    pbl_mutex_lock(&s_lock, PBL_FOREVER);
    prv_load();
    n = (i < s_count) ? PBL_MIN(CHUNK, s_count - i) : 0U;
    ok = (n > 0U) && (pbl_fs_open(&f, prv_log(s_st.log_shift), PBL_FS_O_READ) >= 0);
    if (ok) {
      ok = prv_read_at(&f, i, buf, n);
      pbl_fs_close(&f);
    }
    pbl_mutex_unlock(&s_lock);
    if (!ok) {
      break;
    }

    for (size_t j = 0U; j < n; j++) {
      const struct bt_record *r = &buf[j];

      pbl_shell_print(sh,
                      "%" PRIu32 ",%u,%u,%" PRId32 ",%" PRId32 ",%u,%" PRIu32 ",%" PRIu32
                      ",%u,%" PRId32 ",%u,%u,%u,%" PRId32 ",%" PRId32,
                      r->time, r->mv, r->avg_mv, (int32_t)r->i_10ua * 10,
                      (int32_t)r->avg_i_10ua * 10, r->soc_cpct, (uint32_t)r->remaining_dmah * 100U,
                      (uint32_t)r->full_dmah * 100U, r->counter, (int32_t)r->temp_dc * 100,
                      r->flags, r->phase, r->loads, r->out_uah, r->sw_uah);
    }
  }

  return 0;
}

static int prv_cmd_start(const struct pbl_shell *sh, size_t argc, char **argv) {
  battery_test_start();
  return 0;
}

static int prv_cmd_cancel(const struct pbl_shell *sh, size_t argc, char **argv) {
  battery_test_cancel();
  return 0;
}

PBL_SHELL_SUBCMD_SET_CREATE(sub_battery_test);
PBL_SHELL_SUBCMD_ADD(sub_battery, test, sub_battery_test, "Battery capacity test", nullptr, 0, 0);
PBL_SHELL_SUBCMD_ADD(sub_battery_test, status, nullptr, "Print the phase or the results",
                     prv_cmd_status, 0, 0);
PBL_SHELL_SUBCMD_ADD(sub_battery_test, log, nullptr, "Print the log as CSV", prv_cmd_log, 0, 0);
PBL_SHELL_SUBCMD_ADD(sub_battery_test, start, nullptr, "Start a new test", prv_cmd_start, 0, 0);
PBL_SHELL_SUBCMD_ADD(sub_battery_test, cancel, nullptr, "Cancel the test, discard its results",
                     prv_cmd_cancel, 0, 0);
#endif
