/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <pbl/drivers/gnss.h>
#include <pbl/kernel/mutex.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/services/location.h>

PBL_LOG_MODULE_DEFINE(service_location, CONFIG_SERVICE_LOCATION_LOG_LEVEL);

#define EPOCH_INTERVAL_MS 1000U
/* A position older than this is no longer a fix. */
#define FIX_MAX_AGE_MS 3000U

/* Subscriptions; taken before the receiver's lock, never from its listener. */
static PBL_MUTEX_DEFINE(s_sub_lock);
static PebbleTaskBitset s_subscribers;

/* Epoch history, written from the receiver's listener. */
static PBL_MUTEX_DEFINE(s_lock);
static struct pbl_gnss_fix s_history[LOCATION_SERVICE_HISTORY];
static uint32_t s_count;
static uint64_t s_last_rx_ms;
static bool s_running;

static void prv_fix(const struct pbl_gnss *gnss, const struct pbl_gnss_fix *fix, void *user_data) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_history[s_count % LOCATION_SERVICE_HISTORY] = *fix;
  s_count++;
  s_last_rx_ms = pbl_uptime_ms();
  pbl_mutex_unlock(&s_lock);
}

static const struct pbl_gnss_listener s_listener = {.fix = prv_fix};

bool location_service_is_available(void) {
  return pbl_device_is_ready(&GNSS->dev);
}

static void prv_update(PebbleTaskBitset subscribers) {
  const bool want = subscribers != 0;
  int ret = 0;

  if (want == s_running) {
    return;
  }

  if (want) {
    pbl_gnss_set_listener(GNSS, &s_listener);
    ret = pbl_gnss_start(GNSS, EPOCH_INTERVAL_MS);
  } else {
    ret = pbl_gnss_stop(GNSS);
  }

  if (ret != 0) {
    PBL_LOG_ERR("could not %s the receiver (%d)", want ? "start" : "stop", ret);
    return;
  }

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_running = want;
  pbl_mutex_unlock(&s_lock);
  PBL_LOG_DBG("receiver %s", want ? "on" : "off");
}

static void prv_set_subscribed(PebbleTask task, bool subscribed) {
  pbl_mutex_lock(&s_sub_lock, PBL_FOREVER);
  if (subscribed) {
    s_subscribers |= (PebbleTaskBitset)(1U << task);
  } else {
    s_subscribers &= (PebbleTaskBitset) ~(1U << task);
  }
  prv_update(s_subscribers);
  pbl_mutex_unlock(&s_sub_lock);
}

int location_service_start(void) {
  if (!location_service_is_available()) {
    return -ENODEV;
  }

  prv_set_subscribed(pebble_task_get_current(), true);

  return s_running ? 0 : -EIO;
}

void location_service_stop(void) {
  if (location_service_is_available()) {
    prv_set_subscribed(pebble_task_get_current(), false);
  }
}

void location_service_process_cleanup(PebbleTask task) {
  if (location_service_is_available()) {
    prv_set_subscribed(task, false);
  }
}

LocationStatus location_service_get_status(void) {
  LocationStatus status;

  if (!location_service_is_available()) {
    return LocationStatus_Unavailable;
  }

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (!s_running) {
    status = LocationStatus_Off;
  } else if (s_count > 0 &&
             s_history[(s_count - 1) % LOCATION_SERVICE_HISTORY].quality != PBL_GNSS_FIX_NONE &&
             pbl_uptime_ms() - s_last_rx_ms <= FIX_MAX_AGE_MS) {
    status = LocationStatus_Fix;
  } else {
    status = LocationStatus_Searching;
  }
  pbl_mutex_unlock(&s_lock);

  return status;
}

bool location_service_peek(struct pbl_gnss_fix *fix) {
  bool ret = false;

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_running && s_count > 0) {
    *fix = s_history[(s_count - 1) % LOCATION_SERVICE_HISTORY];
    ret = true;
  }
  pbl_mutex_unlock(&s_lock);

  return ret;
}

uint32_t location_service_get_cursor(void) {
  uint32_t count;

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  count = s_count;
  pbl_mutex_unlock(&s_lock);

  return count;
}

size_t location_service_read(uint32_t *cursor, struct pbl_gnss_fix *fixes, size_t max) {
  size_t n = 0;

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_count - *cursor > LOCATION_SERVICE_HISTORY) {
    *cursor = s_count - LOCATION_SERVICE_HISTORY;
  }
  while (*cursor != s_count && n < max) {
    fixes[n++] = s_history[*cursor % LOCATION_SERVICE_HISTORY];
    (*cursor)++;
  }
  pbl_mutex_unlock(&s_lock);

  return n;
}
