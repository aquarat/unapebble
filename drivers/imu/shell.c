/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <pbl/drivers/accel.h>
#include <pbl/drivers/mag.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/time.h>
#include <pbl/kernel/work.h>
#include <pbl/shell/shell.h>
#include <pbl/util/math.h>

static int prv_accel_read(const struct pbl_shell *sh, size_t argc, char **argv) {
  struct pbl_accel_sample sample;
  int ret;

  ret = pbl_accel_peek(ACCEL, &sample);
  if (ret != 0) {
    pbl_shell_error(sh, "read failed (%d)", ret);
    return ret;
  }

  pbl_shell_print(sh, "x=%" PRId16 " y=%" PRId16 " z=%" PRId16 " mg", sample.x, sample.y, sample.z);
  return 0;
}

static int prv_accel_info(const struct pbl_shell *sh, size_t argc, char **argv) {
  pbl_shell_print(sh, "sampling interval: %" PRIu32 " us", pbl_accel_get_sampling_interval(ACCEL));
  pbl_shell_print(sh, "max FIFO samples: %" PRIu32, pbl_accel_get_max_num_samples(ACCEL));
  pbl_shell_print(sh, "shake detection: %s", pbl_accel_get_shake_detection(ACCEL) ? "on" : "off");
  pbl_shell_print(sh, "double tap detection: %s",
                  pbl_accel_get_double_tap_detection(ACCEL) ? "on" : "off");
  return 0;
}

// What a stream brought: counts, and per axis the sums for the mean and the noise
static struct {
  uint32_t batches;
  uint32_t samples;
  uint32_t shakes;
  int64_t sum[3];
  int64_t sum_sq[3];
  int16_t min[3];
  int16_t max[3];
} s_stream;

static void prv_stream_samples(const struct pbl_accel *accel, const AccelRawBatch *batch,
                               void *user_data) {
  for (uint16_t i = 0U; i < batch->num_samples; i++) {
    const uint8_t *sample = &batch->data[i * batch->stride];

    for (unsigned int axis = 0U; axis < 3U; axis++) {
      const uint8_t *p = &sample[batch->axis[axis].offset];
      int32_t raw = (int16_t)(p[0] | (p[1] << 8)) * batch->axis[axis].sign;
      int16_t mg = (int16_t)((raw * batch->scale_num) / batch->scale_den);

      if (s_stream.samples == 0U) {
        s_stream.min[axis] = mg;
        s_stream.max[axis] = mg;
      }
      s_stream.sum[axis] += mg;
      s_stream.sum_sq[axis] += (int64_t)mg * mg;
      s_stream.min[axis] = PBL_MIN(s_stream.min[axis], mg);
      s_stream.max[axis] = PBL_MAX(s_stream.max[axis], mg);
    }
    s_stream.samples++;
  }
  s_stream.batches++;
}

static void prv_stream_event(const struct pbl_accel *accel, const struct pbl_accel_event *event,
                             void *user_data) {
  if (event->type == PBL_ACCEL_EVENT_SHAKE) {
    s_stream.shakes++;
  }
}

static uint32_t prv_isqrt(uint64_t v) {
  uint64_t r = 0U;
  uint64_t bit = 1ULL << 62;

  while (bit > v) {
    bit >>= 2;
  }
  while (bit != 0U) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }

  return (uint32_t)r;
}

// A stream takes the samples in place of the accelerometer service, which gets them back after
static struct {
  struct pbl_delayed_work stop;
  bool stop_ready;
  bool running;
  bool done;
  uint32_t interval_us;
  uint32_t num_samples;
  uint64_t start_ms;
  uint64_t end_ms;
  const struct pbl_accel_listener *prev_listener;
  uint32_t prev_interval_us;
  uint32_t prev_num_samples;
} s_run;

static void prv_stream_restore(void) {
  pbl_accel_lock(ACCEL);
  if (s_run.running) {
    (void)pbl_accel_set_num_samples(ACCEL, s_run.prev_num_samples);
    (void)pbl_accel_set_sampling_interval(ACCEL, s_run.prev_interval_us, nullptr);
    (void)pbl_accel_set_listener(ACCEL, s_run.prev_listener);
    s_run.running = false;
    s_run.done = true;
    s_run.end_ms = pbl_uptime_ms();
  }
  pbl_accel_unlock(ACCEL);
}

static void prv_stream_stop(struct pbl_work *work) {
  prv_stream_restore();
}

static void prv_stream_print(const struct pbl_shell *sh) {
  const uint32_t ms = (uint32_t)((s_run.running ? pbl_uptime_ms() : s_run.end_ms) - s_run.start_ms);
  uint32_t samples, centihz;

  pbl_accel_lock(ACCEL);
  samples = s_stream.samples;
  centihz = (ms == 0U) ? 0U : (uint32_t)(((uint64_t)samples * 100000U) / ms);
  pbl_shell_print(sh, "%s, %" PRIu32 " ms at %" PRIu32 " us in batches of %" PRIu32,
                  s_run.running ? "streaming" : "done", ms, s_run.interval_us, s_run.num_samples);
  pbl_shell_print(sh,
                  "%" PRIu32 " batches, %" PRIu32 " samples (%" PRIu32 ".%02" PRIu32
                  " Hz), %" PRIu32 " shakes",
                  s_stream.batches, samples, centihz / 100U, centihz % 100U, s_stream.shakes);

  for (unsigned int axis = 0U; samples > 0U && axis < 3U; axis++) {
    const int64_t n = samples;
    const int64_t mean = s_stream.sum[axis] / n;
    // Variance in hundredths of mg^2, for the noise in tenths of mg
    const int64_t var_100 =
        ((s_stream.sum_sq[axis] * 100) - ((s_stream.sum[axis] * s_stream.sum[axis] * 100) / n)) / n;
    const uint32_t std_10 = prv_isqrt(var_100 > 0 ? (uint64_t)var_100 : 0U);

    pbl_shell_print(sh,
                    "%c: mean %" PRId32 " mg, noise %" PRIu32 ".%" PRIu32 " mg rms, %" PRId16
                    " to %" PRId16,
                    'x' + (char)axis, (int32_t)mean, std_10 / 10U, std_10 % 10U, s_stream.min[axis],
                    s_stream.max[axis]);
  }
  pbl_accel_unlock(ACCEL);
}

// stream SECONDS [INTERVAL_US [BATCH]] starts one, stream alone shows it, stream stop ends it
static int prv_accel_stream(const struct pbl_shell *sh, size_t argc, char **argv) {
  static const struct pbl_accel_listener listener = {
    .samples = prv_stream_samples,
    .event = prv_stream_event,
  };
  unsigned long seconds = 0UL, interval_us = 40000UL, batch = 25UL;
  int ret;

  if (argc == 1) {
    if (!s_run.running && !s_run.done) {
      pbl_shell_print(sh, "no stream: stream SECONDS [INTERVAL_US [BATCH]]");
      return 0;
    }
    prv_stream_print(sh);
    return 0;
  }

  if (argc == 2 && strcmp(argv[1], "stop") == 0) {
    if (s_run.stop_ready) {
      (void)pbl_delayed_work_cancel(&s_run.stop);
    }
    prv_stream_restore();
    prv_stream_print(sh);
    return 0;
  }

  if ((pbl_shell_strtoul(argv[1], &seconds) != 0) ||
      (argc > 2 && pbl_shell_strtoul(argv[2], &interval_us) != 0) ||
      (argc > 3 && pbl_shell_strtoul(argv[3], &batch) != 0) || seconds == 0UL || seconds > 3600UL ||
      interval_us == 0UL) {
    pbl_shell_error(sh, "usage: stream [SECONDS [INTERVAL_US [BATCH]] | stop]");
    return -EINVAL;
  }

  if (s_run.running) {
    pbl_shell_error(sh, "already streaming");
    return -EBUSY;
  }

  if (!s_run.stop_ready) {
    pbl_delayed_work_init(&s_run.stop, prv_stream_stop);
    s_run.stop_ready = true;
  }

  pbl_accel_lock(ACCEL);
  s_run.prev_listener = ACCEL->state->listener;
  s_run.prev_interval_us = pbl_accel_get_sampling_interval(ACCEL);
  s_run.prev_num_samples = pbl_accel_get_num_samples(ACCEL);
  memset(&s_stream, 0, sizeof(s_stream));
  (void)pbl_accel_set_listener(ACCEL, &listener);
  s_run.running = true;
  s_run.done = false;
  s_run.start_ms = pbl_uptime_ms();
  ret = pbl_accel_set_sampling_interval(ACCEL, interval_us, &s_run.interval_us);
  if (ret == 0) {
    ret = pbl_accel_set_num_samples(ACCEL, PBL_MIN(batch, pbl_accel_get_max_num_samples(ACCEL)));
  }
  s_run.num_samples = pbl_accel_get_num_samples(ACCEL);
  pbl_accel_unlock(ACCEL);

  if (ret != 0) {
    prv_stream_restore();
    pbl_shell_error(sh, "could not stream (%d)", ret);
    return ret;
  }

  (void)pbl_delayed_work_reschedule(&pbl_sys_workq, &s_run.stop, PBL_MSEC(seconds * 1000UL));
  pbl_shell_print(sh,
                  "streaming %lu s at %" PRIu32 " us in batches of %" PRIu32
                  ": 'imu accel stream' shows it",
                  seconds, s_run.interval_us, s_run.num_samples);

  return 0;
}

static const struct pbl_shell_cmd sub_accel[] = {
  PBL_SHELL_CMD(read, nullptr, "Read one sample", prv_accel_read),
  PBL_SHELL_CMD(info, nullptr, "Show the driver state", prv_accel_info),
  PBL_SHELL_CMD(stream, nullptr,
                "Take the samples for SECONDS at INTERVAL_US (40000) in batches of BATCH (25), "
                "alone show the rate, mean and noise, or stop",
                prv_accel_stream),
  PBL_SHELL_SUBCMD_SET_END,
};

#ifdef CONFIG_MAG
static int prv_mag_read(const struct pbl_shell *sh, size_t argc, char **argv) {
  bool sampling = pbl_mag_get_sampling_interval(MAG) != 0U;
  MagData sample;
  int ret;

  // Sample briefly unless another user already is
  if (!sampling) {
    ret = pbl_mag_set_sampling_interval(MAG, 200000U, nullptr);
    if (ret != 0) {
      pbl_shell_error(sh, "could not start sampling (%d)", ret);
      return ret;
    }

    pbl_sleep_ms(250);
  }

  ret = pbl_mag_peek(MAG, &sample);

  if (!sampling) {
    (void)pbl_mag_set_sampling_interval(MAG, 0U, nullptr);
  }

  if (ret != 0) {
    pbl_shell_error(sh, "read failed (%d)", ret);
    return ret;
  }

  pbl_shell_print(sh, "x=%" PRId16 " y=%" PRId16 " z=%" PRId16 " mG", sample.x, sample.y, sample.z);
  return 0;
}

static int prv_mag_info(const struct pbl_shell *sh, size_t argc, char **argv) {
  pbl_shell_print(sh, "sampling interval: %" PRIu32 " us", pbl_mag_get_sampling_interval(MAG));
  return 0;
}

static const struct pbl_shell_cmd sub_mag[] = {
  PBL_SHELL_CMD(read, nullptr, "Read one sample", prv_mag_read),
  PBL_SHELL_CMD(info, nullptr, "Show the driver state", prv_mag_info),
  PBL_SHELL_SUBCMD_SET_END,
};
#endif

static const struct pbl_shell_cmd sub_imu[] = {
  PBL_SHELL_CMD(accel, sub_accel, "Accelerometer", nullptr),
#ifdef CONFIG_MAG
  PBL_SHELL_CMD(mag, sub_mag, "Magnetometer", nullptr),
#endif
  PBL_SHELL_SUBCMD_SET_END,
};

PBL_SHELL_CMD_REGISTER(imu, sub_imu, "IMU drivers", nullptr);
