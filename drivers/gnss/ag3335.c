/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "nmea.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/gnss/ag3335.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/sem.h>
#include <pbl/kernel/time.h>
#include <pbl/kernel/work.h>
#include <pbl/logging/logging.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

PBL_LOG_MODULE_DEFINE(gnss_ag3335, CONFIG_DRIVER_GNSS_LOG_LEVEL);

/*
 * Start-up, as UNA's kernel does it (AirohaBase.cpp, GpsAiroha.cpp): power, then the receiver's
 * "GNSS subsystem on" ($PAIR001,002,0) before anything is sent; its system sleep locked, its GNSS
 * subsystem off for the band to change, on again, a hot start. Each command is acknowledged with
 * $PAIR001,<command>,<result>, 0 for success, 1 while it is processed.
 */
#define BOOT_TIMEOUT_MS  3000U
#define ACK_TIMEOUT_MS   500U
#define START_TIMEOUT_MS 2000U
// The receiver loses a command now and then, at least once its GNSS subsystem is off or its system
// may sleep: the first one wakes it up. A command goes out this many times without an answer.
#define SENDS         4U
#define BOOT_ATTEMPTS 2U
// Off for this long at least, for the receiver to reset
#define OFF_MIN_MS 500U
// The line quiet this long ends the burst of an epoch
#define QUIET_MS 100U
// A query's answer comes within this of its acknowledgement
#define REPLY_WAIT_MS 200U

#define PAIR_ACK_PROCESSING 1

// The high priority queue, as the shell's commands run on the system work queue and wait for the
// receiver
#define WORKQ (&pbl_sys_workq_high)

enum state {
  STATE_OFF,
  STATE_BOOT,
  STATE_RETRY,
  STATE_CONFIG,
  STATE_TRACKING,
  STATE_FAILED,
};

static const char *const s_state_names[] = {
  [STATE_OFF] = "off",
  [STATE_BOOT] = "starting",
  [STATE_RETRY] = "starting again",
  [STATE_CONFIG] = "configuring",
  [STATE_TRACKING] = "tracking",
  [STATE_FAILED] = "no answer",
};

#define MAX_STEPS 10U
#define CMD_LEN   48U
#define STEP_LEN  32U

// Commands a caller may send: queries, and settings of the running receiver, none to its flash
static const char *const s_allowed[] = {
  "002", "003", "004", "005", "006", "020", "021", "050", "051", "058", "059", "062", "063",
  "064", "065", "066", "067", "068", "069", "072", "073", "080", "081", "104", "105", "382",
};

struct pbl_gnss_ag3335_data {
  // Written by the UART's interrupt, read by the work
  uint8_t ring[PBL_GNSS_AG3335_RX_RING_SIZE];
  volatile uint32_t head;
  volatile uint32_t tail;
  volatile uint32_t overruns;
  volatile uint32_t rx_bytes;

  struct pbl_work rx_work;
  struct pbl_delayed_work quiet_work;
  struct pbl_delayed_work timeout_work;
  const struct pbl_gnss *gnss;

  char line[NMEA_MAX_LEN + 1];
  uint32_t line_len;
  bool line_skip;

  enum state state;
  uint8_t boot_attempts;
  uint8_t step;
  uint8_t num_steps;
  char steps[MAX_STEPS][STEP_LEN];
  // The command waiting for its acknowledgement, "" for none, and how often it went out
  char waiting[4];
  char cmd[CMD_LEN];
  uint8_t sends;

  uint32_t interval_ms;
  uint32_t every;
  uint32_t epoch_count;
  uint64_t start_ms;
  uint64_t off_ms;

  struct nmea_epoch epoch;
  struct nmea_epoch done;

  // A caller's command
  struct pbl_sem user_done;
  bool user_waiting;
  int user_result;
  char *user_reply;
  size_t user_reply_len;
  char user_cmd[4];

  char version[64];
  uint32_t sentences[PBL_GNSS_SENTENCE_COUNT];
  uint32_t bad;
  uint32_t epochs;
  uint8_t sats_tracked;
  uint8_t best_cn0;
  uint32_t ttff_ms;
};

struct pbl_gnss_ag3335_data pbl_gnss_ag3335_data;

static const struct pbl_gnss_ag3335 *prv_dev(const struct pbl_gnss *gnss) {
  return PBL_CONTAINER_OF(gnss, const struct pbl_gnss_ag3335, gnss);
}

static void prv_send(const struct pbl_gnss_ag3335 *ag, const char *body) {
  char tail[6];

  (void)snprintf(tail, sizeof(tail), "*%02X\r\n", nmea_checksum(body));

  pbl_uart_poll_out(ag->uart, '$');
  for (const char *p = body; *p != '\0'; p++) {
    pbl_uart_poll_out(ag->uart, (uint8_t)*p);
  }
  for (const char *p = tail; *p != '\0'; p++) {
    pbl_uart_poll_out(ag->uart, (uint8_t)*p);
  }

  PBL_LOG_DBG("> $%s", body);
}

// Interrupt context: the byte into the ring, the work woken up at the end of a line
static void prv_rx(const struct pbl_uart *uart, uint8_t c, void *arg) {
  struct pbl_gnss_ag3335_data *d = arg;
  uint32_t head = d->head;
  uint32_t next = (head + 1U) % PBL_GNSS_AG3335_RX_RING_SIZE;

  d->rx_bytes++;
  if (next == d->tail) {
    d->overruns++;
    return;
  }

  d->ring[head] = c;
  d->head = next;

  if (c == '\n' ||
      (next - d->tail) % PBL_GNSS_AG3335_RX_RING_SIZE >= PBL_GNSS_AG3335_RX_RING_SIZE / 2U) {
    (void)pbl_work_submit(WORKQ, &d->rx_work);
  }
}

static void prv_power_off(const struct pbl_gnss_ag3335 *ag) {
  struct pbl_gnss_ag3335_data *d = ag->data;

  (void)pbl_uart_rx_disable(ag->uart);
  (void)pbl_gpio_set(&ag->enable, false);
  (void)pbl_delayed_work_cancel(&d->timeout_work);
  (void)pbl_delayed_work_cancel(&d->quiet_work);
  d->off_ms = pbl_uptime_ms();
  d->waiting[0] = '\0';
}

static void prv_power_on(const struct pbl_gnss_ag3335 *ag) {
  struct pbl_gnss_ag3335_data *d = ag->data;
  uint64_t off_for = pbl_uptime_ms() - d->off_ms;

  if (off_for < OFF_MIN_MS) {
    pbl_sleep_ms((uint32_t)(OFF_MIN_MS - off_for));
  }

  pbl_irq_lock();
  d->head = 0U;
  d->tail = 0U;
  pbl_irq_unlock();
  d->line_len = 0U;
  d->line_skip = false;
  nmea_epoch_init(&d->epoch);

  // Powered first, so that the UART's lines do not feed it while it is not
  (void)pbl_gpio_set(&ag->enable, true);
  (void)pbl_uart_rx_enable(ag->uart, prv_rx, d);

  d->state = STATE_BOOT;
  (void)pbl_delayed_work_reschedule(WORKQ, &d->timeout_work, PBL_MSEC(BOOT_TIMEOUT_MS));
}

static void prv_add_step(struct pbl_gnss_ag3335_data *d, const char *fmt, uint32_t arg) {
  if (d->num_steps < MAX_STEPS) {
    (void)snprintf(d->steps[d->num_steps++], STEP_LEN, fmt, arg);
  }
}

// The set-up, for an interval: fixes at it up to 1 Hz, every so many 1 Hz fixes past it
static void prv_plan(struct pbl_gnss_ag3335_data *d) {
  d->num_steps = 0U;
  d->step = 0U;

  prv_add_step(d, "PAIR020", 0U);
  prv_add_step(d, "PAIR382,%" PRIu32, 1U);
  prv_add_step(d, "PAIR003", 0U);
#ifdef CONFIG_GNSS_AG3335_DUAL_BAND
  prv_add_step(d, "PAIR104,%" PRIu32, 1U);
#else
  prv_add_step(d, "PAIR104,%" PRIu32, 0U);
#endif
  if (d->interval_ms < 1000U) {
    prv_add_step(d, "PAIR050,%" PRIu32, PBL_MAX(d->interval_ms, 100U));
  }
  prv_add_step(d, "PAIR002", 0U);
#ifdef CONFIG_GNSS_AG3335_SLEEP
  prv_add_step(d, "PAIR382,%" PRIu32, 0U);
#endif
  prv_add_step(d, "PAIR004", 0U);
}

// A command out, its acknowledgement awaited
static void prv_transmit(const struct pbl_gnss_ag3335 *ag) {
  struct pbl_gnss_ag3335_data *d = ag->data;

  d->sends++;
  prv_send(ag, d->cmd);
  (void)pbl_delayed_work_reschedule(
      WORKQ, &d->timeout_work,
      PBL_MSEC(strncmp(d->waiting, "00", 2U) == 0 ? START_TIMEOUT_MS : ACK_TIMEOUT_MS));
}

static void prv_command(const struct pbl_gnss_ag3335 *ag, const char *cmd) {
  struct pbl_gnss_ag3335_data *d = ag->data;

  (void)snprintf(d->cmd, sizeof(d->cmd), "%s", cmd);
  memcpy(d->waiting, cmd + 4, 3U);
  d->waiting[3] = '\0';
  d->sends = 0U;
  prv_transmit(ag);
}

static void prv_next_step(const struct pbl_gnss_ag3335 *ag) {
  struct pbl_gnss_ag3335_data *d = ag->data;
  const char *step;

  if (d->step >= d->num_steps) {
    d->state = STATE_TRACKING;
    d->waiting[0] = '\0';
    (void)pbl_delayed_work_cancel(&d->timeout_work);
    PBL_LOG_DBG("tracking");
    return;
  }

  step = d->steps[d->step++];
  prv_command(ag, step);
}

static void prv_timeout_fn(struct pbl_work *work) {
  struct pbl_gnss_ag3335_data *d = &pbl_gnss_ag3335_data;
  const struct pbl_gnss *gnss = d->gnss;
  const struct pbl_gnss_ag3335 *ag = prv_dev(gnss);

  pbl_gnss_lock(gnss);

  switch (d->state) {
    case STATE_BOOT:
      prv_power_off(ag);
      if (++d->boot_attempts < BOOT_ATTEMPTS) {
        PBL_LOG_WRN("no answer from the receiver, powering it again");
        d->state = STATE_RETRY;
        (void)pbl_delayed_work_reschedule(WORKQ, &d->timeout_work, PBL_MSEC(OFF_MIN_MS));
      } else {
        PBL_LOG_ERR("no answer from the receiver");
        d->state = STATE_FAILED;
      }
      break;
    case STATE_RETRY:
      prv_power_on(ag);
      break;
    case STATE_CONFIG:
      if (d->sends < SENDS) {
        prv_transmit(ag);
        break;
      }
      PBL_LOG_WRN("no acknowledgement of PAIR%s", d->waiting);
      prv_next_step(ag);
      break;
    case STATE_TRACKING:
      if (d->user_waiting && d->sends < SENDS) {
        prv_transmit(ag);
      } else if (d->user_waiting) {
        d->user_waiting = false;
        d->user_result = -ETIMEDOUT;
        d->waiting[0] = '\0';
        pbl_sem_give(&d->user_done);
      }
      break;
    default:
      break;
  }

  pbl_gnss_unlock(gnss);
}

static void prv_report(const struct pbl_gnss_ag3335 *ag, struct nmea_epoch *ep) {
  struct pbl_gnss_ag3335_data *d = ag->data;

  d->sats_tracked = (uint8_t)PBL_MIN(nmea_sky_tracked(&ep->sky), UINT8_MAX);
  d->best_cn0 = ep->sky.best_cn0;
  if (ep->fix.quality != PBL_GNSS_FIX_NONE && d->ttff_ms == 0U) {
    d->ttff_ms = (uint32_t)(pbl_uptime_ms() - d->start_ms);
    PBL_LOG_INFO("first fix after %" PRIu32 " ms", d->ttff_ms);
  }

  if (d->every > 1U && (d->epoch_count++ % d->every) != 0U) {
    return;
  }

  d->epochs++;
  pbl_gnss_report_fix(&ag->gnss, &ep->fix);
}

static void prv_quiet_fn(struct pbl_work *work) {
  struct pbl_gnss_ag3335_data *d = &pbl_gnss_ag3335_data;
  const struct pbl_gnss *gnss = d->gnss;

  pbl_gnss_lock(gnss);
  if (d->state != STATE_OFF && d->state != STATE_FAILED && nmea_epoch_end(&d->epoch, &d->done)) {
    prv_report(prv_dev(gnss), &d->done);
  }
  pbl_gnss_unlock(gnss);
}

// $PAIR001,<command>,<result>, or the answer of a query, $PAIR<command>,...
static void prv_pair(const struct pbl_gnss_ag3335 *ag, char **f, int n) {
  struct pbl_gnss_ag3335_data *d = ag->data;
  bool ack = strcmp(f[0], "PAIR001") == 0 && n >= 3;
  uint32_t result = 0U;

  if (!ack) {
    // Its firmware, asked for or as it says it after starting
    if ((strcmp(f[0], "PAIR020") == 0 || strcmp(f[0], "PAIR021") == 0) && n >= 2) {
      (void)snprintf(d->version, sizeof(d->version), "%s", f[1]);
    }
    // A query's answer comes after its acknowledgement
    if (d->user_reply != nullptr && strcmp(f[0] + 4, d->user_cmd) == 0) {
      {
        size_t pos = 0U;

        for (int i = 0; i < n && pos + 1U < d->user_reply_len; i++) {
          pos += (size_t)snprintf(d->user_reply + pos, d->user_reply_len - pos, "%s%s",
                                  i > 0 ? "," : "", f[i]);
        }
      }
    }
    return;
  }

  for (const char *p = f[2]; *p >= '0' && *p <= '9'; p++) {
    result = result * 10U + (uint32_t)(*p - '0');
  }

  if (d->state == STATE_BOOT) {
    if (strcmp(f[1], "002") == 0 && result == 0U) {
      PBL_LOG_DBG("receiver on");
      d->state = STATE_CONFIG;
      prv_next_step(ag);
    }
    return;
  }

  if (d->waiting[0] == '\0' || strcmp(f[1], d->waiting) != 0 || result == PAIR_ACK_PROCESSING) {
    return;
  }

  if (d->state == STATE_CONFIG) {
    if (result != 0U) {
      PBL_LOG_WRN("PAIR%s: result %" PRIu32, d->waiting, result);
    }
    prv_next_step(ag);
    return;
  }

  if (d->user_waiting) {
    (void)pbl_delayed_work_cancel(&d->timeout_work);
    d->user_waiting = false;
    d->user_result = (int)result;
    d->waiting[0] = '\0';
    pbl_sem_give(&d->user_done);
  }
}

static void prv_line(const struct pbl_gnss_ag3335 *ag, char *line) {
  struct pbl_gnss_ag3335_data *d = ag->data;
  char *fields[NMEA_MAX_FIELDS];
  enum nmea_type type;
  int n;

  if (!nmea_checksum_ok(line)) {
    d->bad++;
    PBL_LOG_DBG("bad: %s", line);
    return;
  }

  if (strncmp(line, "$PAIR", 5U) == 0) {
    PBL_LOG_DBG("< %s", line);
  }

  n = nmea_split(line, fields, PBL_ARRAY_SIZE(fields));
  if (n < 1) {
    d->bad++;
    return;
  }

  type = nmea_type_of(fields[0]);
  switch (type) {
    case NMEA_GGA:
      d->sentences[PBL_GNSS_SENTENCE_GGA]++;
      break;
    case NMEA_RMC:
      d->sentences[PBL_GNSS_SENTENCE_RMC]++;
      break;
    case NMEA_GSA:
      d->sentences[PBL_GNSS_SENTENCE_GSA]++;
      break;
    case NMEA_GSV:
      d->sentences[PBL_GNSS_SENTENCE_GSV]++;
      break;
    case NMEA_PAIR:
      d->sentences[PBL_GNSS_SENTENCE_PROPRIETARY]++;
      prv_pair(ag, fields, n);
      return;
    default:
      d->sentences[PBL_GNSS_SENTENCE_OTHER]++;
      return;
  }

  if (nmea_epoch_add(&d->epoch, fields, n, &d->done)) {
    prv_report(ag, &d->done);
  }
}

// Lines out of the ring: printable characters from a '$' to the end of the line
static void prv_rx_fn(struct pbl_work *work) {
  struct pbl_gnss_ag3335_data *d = &pbl_gnss_ag3335_data;
  const struct pbl_gnss *gnss = d->gnss;
  const struct pbl_gnss_ag3335 *ag = prv_dev(gnss);
  bool lines = false;

  pbl_gnss_lock(gnss);

  while (d->tail != d->head) {
    char c = (char)d->ring[d->tail];

    d->tail = (d->tail + 1U) % PBL_GNSS_AG3335_RX_RING_SIZE;

    if (d->state == STATE_OFF || d->state == STATE_FAILED) {
      continue;
    }

    if (c == '$') {
      if (d->line_len > 0U && !d->line_skip) {
        d->bad++;
      }
      d->line[0] = '$';
      d->line_len = 1U;
      d->line_skip = false;
      continue;
    }

    if (c == '\r' || c == '\n') {
      if (d->line_len > 0U && !d->line_skip) {
        d->line[d->line_len] = '\0';
        prv_line(ag, d->line);
        lines = true;
      }
      d->line_len = 0U;
      d->line_skip = false;
      continue;
    }

    if (d->line_len == 0U || d->line_skip) {
      continue;
    }

    if (c < 0x20 || c > 0x7e || d->line_len >= NMEA_MAX_LEN) {
      d->bad++;
      d->line_skip = true;
      continue;
    }

    d->line[d->line_len++] = c;
  }

  if (lines && d->state != STATE_OFF && d->state != STATE_FAILED) {
    (void)pbl_delayed_work_reschedule(WORKQ, &d->quiet_work, PBL_MSEC(QUIET_MS));
  }

  pbl_gnss_unlock(gnss);
}

static int prv_init(const struct pbl_gnss *gnss) {
  const struct pbl_gnss_ag3335 *ag = prv_dev(gnss);
  struct pbl_gnss_ag3335_data *d = ag->data;
  int ret;

  d->gnss = gnss;
  d->state = STATE_OFF;
  pbl_work_init(&d->rx_work, prv_rx_fn);
  pbl_delayed_work_init(&d->quiet_work, prv_quiet_fn);
  pbl_delayed_work_init(&d->timeout_work, prv_timeout_fn);
  pbl_sem_init(&d->user_done, 0U, 1U);

  ret = pbl_device_init(&ag->enable.port->dev);
  if (ret == 0) {
    ret = pbl_gpio_configure(&ag->enable, PBL_GPIO_OUTPUT_INACTIVE);
  }
  if (ret != 0) {
    return ret;
  }

  ret = pbl_device_init(&ag->uart->dev);
  if (ret != 0) {
    return ret;
  }

  d->off_ms = pbl_uptime_ms();

  return 0;
}

static int prv_start(const struct pbl_gnss *gnss, uint32_t interval_ms) {
  const struct pbl_gnss_ag3335 *ag = prv_dev(gnss);
  struct pbl_gnss_ag3335_data *d = ag->data;
  bool running = d->state != STATE_OFF && d->state != STATE_FAILED;

  d->every = interval_ms > 1000U ? (interval_ms + 500U) / 1000U : 1U;
  d->epoch_count = 0U;

  // The rate changes with the next start, past 1 Hz
  if (running && (interval_ms >= 1000U) == (d->interval_ms >= 1000U)) {
    d->interval_ms = interval_ms;
    return 0;
  }

  if (running) {
    prv_power_off(ag);
  }

  d->interval_ms = interval_ms;
  d->boot_attempts = 0U;
  d->start_ms = pbl_uptime_ms();
  d->ttff_ms = 0U;
  d->sats_tracked = 0U;
  d->best_cn0 = 0U;
  prv_plan(d);
  prv_power_on(ag);

  return 0;
}

static int prv_stop(const struct pbl_gnss *gnss) {
  const struct pbl_gnss_ag3335 *ag = prv_dev(gnss);
  struct pbl_gnss_ag3335_data *d = ag->data;

  prv_power_off(ag);
  d->state = STATE_OFF;
  if (d->user_waiting) {
    d->user_waiting = false;
    d->user_result = -ENODEV;
    pbl_sem_give(&d->user_done);
  }

  return 0;
}

static void prv_get_stats(const struct pbl_gnss *gnss, struct pbl_gnss_stats *stats) {
  struct pbl_gnss_ag3335_data *d = prv_dev(gnss)->data;

  stats->state = s_state_names[d->state];
  stats->version = d->version[0] != '\0' ? d->version : nullptr;
  stats->rx_bytes = d->rx_bytes;
  memcpy(stats->sentences, d->sentences, sizeof(stats->sentences));
  stats->bad = d->bad;
  stats->overruns = d->overruns;
  stats->epochs = d->epochs;
  stats->sats_tracked = d->sats_tracked;
  stats->best_cn0 = d->best_cn0;
  stats->time_to_fix_ms = d->ttff_ms;
}

static bool prv_allowed(const char *cmd) {
  if (strncmp(cmd, "PAIR", 4U) != 0 || strlen(cmd) < 7U || (cmd[7] != '\0' && cmd[7] != ',')) {
    return false;
  }

  for (size_t i = 0U; i < PBL_ARRAY_SIZE(s_allowed); i++) {
    if (strncmp(cmd + 4, s_allowed[i], 3U) == 0) {
      return true;
    }
  }

  return false;
}

int pbl_gnss_ag3335_command(const struct pbl_gnss *gnss, const char *cmd, char *reply,
                            size_t reply_len) {
  const struct pbl_gnss_ag3335 *ag = prv_dev(gnss);
  struct pbl_gnss_ag3335_data *d = ag->data;
  int ret;

  if (!prv_allowed(cmd) || strchr(cmd, '*') != nullptr || strchr(cmd, '$') != nullptr ||
      strlen(cmd) >= CMD_LEN) {
    return -EPERM;
  }

  if (reply != nullptr && reply_len > 0U) {
    reply[0] = '\0';
  }

  pbl_gnss_lock(gnss);
  if (d->state != STATE_TRACKING) {
    ret = d->state == STATE_OFF || d->state == STATE_FAILED ? -ENODEV : -EBUSY;
    pbl_gnss_unlock(gnss);
    return ret;
  }
  if (d->user_waiting) {
    pbl_gnss_unlock(gnss);
    return -EBUSY;
  }

  pbl_sem_reset(&d->user_done);
  d->user_waiting = true;
  d->user_reply = reply;
  d->user_reply_len = reply_len;
  memcpy(d->user_cmd, cmd + 4, 3U);
  d->user_cmd[3] = '\0';
  prv_command(ag, cmd);
  pbl_gnss_unlock(gnss);

  // The last timeout gives it back, or stopping
  (void)pbl_sem_take(&d->user_done, PBL_MSEC(SENDS * START_TIMEOUT_MS + 1000U));

  // The answer of a query follows its acknowledgement
  if (reply != nullptr && reply_len > 0U && reply[0] == '\0') {
    pbl_sleep_ms(REPLY_WAIT_MS);
  }

  pbl_gnss_lock(gnss);
  ret = d->user_result;
  if (d->user_waiting) {
    d->user_waiting = false;
    d->waiting[0] = '\0';
    ret = -ETIMEDOUT;
  }
  d->user_reply = nullptr;
  pbl_gnss_unlock(gnss);

  return ret;
}

const struct pbl_gnss_ops pbl_gnss_ag3335_ops = {
  .init = prv_init,
  .start = prv_start,
  .stop = prv_stop,
  .get_stats = prv_get_stats,
};

#include PBL_DT_INSTANCES(airoha_ag3335m)
