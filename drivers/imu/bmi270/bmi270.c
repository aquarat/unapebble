/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "bmi270_regs.h"

#include <errno.h>
#include <inttypes.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/accel.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/i2c.h>
#include <pbl/drivers/imu/bmi270/bmi270.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/time.h>
#include <pbl/kernel/work.h>
#include <pbl/logging/logging.h>
#include <pbl/services/imu/units.h>
#include <pbl/util/byteorder.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

#ifdef CONFIG_IMU_SHELL
#include <pbl/shell/shell.h>
#endif

PBL_LOG_MODULE_DECLARE(driver_accel, CONFIG_DRIVER_IMU_LOG_LEVEL);

// Implementation notes:
//
// - The accelerometer runs in low power mode, averaging CONFIG_ACCEL_BMI270_AVG_LOG2 samples, with
//   advanced power save on whenever the driver is not configuring it: in it the part takes one
//   register a write and needs 450 us after every access, so the driver turns it off around its
//   configuration and waits after each read.
// - The FIFO holds accelerometer frames without headers, read at its watermark. Shakes are the
//   feature engine's any-motion, which runs at 50 Hz: with shakes on, the part samples at 50 Hz at
//   least and the FIFO down-samples to the rate asked for.
// - Interrupts are latched on the pin, active high, until their status is read. The work item
//   reads the status first, then the FIFO, and runs again while the pad stays high. A watchdog
//   services the FIFO should its interrupt be late.
// - Every reconfiguration reports what the FIFO holds, then flushes it.

// The one instance, for the interrupt handler
static const struct pbl_bmi270 *s_dev;

#define SOFTRESET_MS       2U
#define APS_ACCESS_US      450U
#define INIT_POLL_MS       10U
#define INIT_TIMEOUT_MS    150U
#define CONFIG_CHUNK_BYTES 256U
#define PEEK_POLL_MS       5U
#define PEEK_TIMEOUT_MS    100U
#define DRAIN_READS_MAX    4U
#define WATCHDOG_MIN_MS    2000U
#define S16_RANGE          32768

#if CONFIG_ACCEL_BMI270_SCALE_MG == 2000
#define ACC_RANGE BMI270_ACC_RANGE_2G
#elif CONFIG_ACCEL_BMI270_SCALE_MG == 4000
#define ACC_RANGE BMI270_ACC_RANGE_4G
#elif CONFIG_ACCEL_BMI270_SCALE_MG == 8000
#define ACC_RANGE BMI270_ACC_RANGE_8G
#elif CONFIG_ACCEL_BMI270_SCALE_MG == 16000
#define ACC_RANGE BMI270_ACC_RANGE_16G
#else
#error "CONFIG_ACCEL_BMI270_SCALE_MG must be 2000, 4000, 8000 or 16000"
#endif

////////////////////////////////////////////////////////////////////////////////
// Bus
////////////////////////////////////////////////////////////////////////////////

// In advanced power save the part needs 450 us after every access, a read as a write, as Bosch's
// SensorAPI waits
static void prv_access_done(void) {
  if (s_dev->state->aps) {
    pbl_busy_wait_us(APS_ACCESS_US);
  }
}

static int prv_read(uint8_t reg, void *buf, uint16_t len) {
  int ret = pbl_i2c_burst_read(&s_dev->i2c, reg, buf, len);

  if (ret != 0) {
    s_dev->state->stats.errors++;
    PBL_LOG_ERR("Could not read register 0x%02x (%d)", reg, ret);
  }
  prv_access_done();

  return ret;
}

// In advanced power save a write takes a single register
static int prv_write(uint8_t reg, const void *buf, uint16_t len) {
  int ret = pbl_i2c_burst_write(&s_dev->i2c, reg, buf, len);

  if (ret != 0) {
    s_dev->state->stats.errors++;
    PBL_LOG_ERR("Could not write register 0x%02x (%d)", reg, ret);
  }
  prv_access_done();

  return ret;
}

static int prv_write_byte(uint8_t reg, uint8_t val) {
  return prv_write(reg, &val, 1U);
}

static int prv_set_aps(bool on) {
  int ret;

  if (s_dev->state->aps == on) {
    return 0;
  }

  ret = prv_write_byte(BMI270_PWR_CONF,
                       BMI270_PWR_CONF_FIFO_SELF_WAKE | (on ? BMI270_PWR_CONF_ADV_POWER_SAVE : 0U));
  if (ret == 0) {
    s_dev->state->aps = on;
  }

  return ret;
}

static uint8_t prv_pin_reg(uint8_t int1_reg, uint8_t int2_reg) {
  return (s_dev->int_pin == 2U) ? int2_reg : int1_reg;
}

////////////////////////////////////////////////////////////////////////////////
// Samples
////////////////////////////////////////////////////////////////////////////////

static int8_t prv_sign(IMUCoordinateAxis axis) {
  int8_t sign = s_dev->axis_dir[axis];

  if (s_dev->state->rotated && (axis == AXIS_X || axis == AXIS_Y)) {
    sign = (int8_t)-sign;
  }

  return sign;
}

static void prv_frame_to_mg(const uint8_t *frame, int16_t mg[3]) {
  for (IMUCoordinateAxis axis = AXIS_X; axis <= AXIS_Z; axis++) {
    int32_t raw = (int16_t)pbl_get_le16(&frame[s_dev->axis_map[axis] * 2U]);

    mg[axis] = (int16_t)((prv_sign(axis) * raw * CONFIG_ACCEL_BMI270_SCALE_MG) / S16_RANGE);
  }
}

static uint32_t prv_interval_us(void) {
  return BMI270_ODR_US(s_dev->state->fifo_odr);
}

static void prv_report_frames(uint16_t frames) {
  struct pbl_bmi270_state *state = s_dev->state;
  const uint32_t interval_us = prv_interval_us();
  const uint64_t now_us = pbl_realtime_ms() * 1000ULL;
  AccelRawBatch batch = {
    .data = state->buf,
    .num_samples = frames,
    .stride = BMI270_FRAME_BYTES,
    .axis =
        {
          [AXIS_X] = {.offset = s_dev->axis_map[AXIS_X] * 2U, .sign = prv_sign(AXIS_X)},
          [AXIS_Y] = {.offset = s_dev->axis_map[AXIS_Y] * 2U, .sign = prv_sign(AXIS_Y)},
          [AXIS_Z] = {.offset = s_dev->axis_map[AXIS_Z] * 2U, .sign = prv_sign(AXIS_Z)},
        },
    .scale_num = CONFIG_ACCEL_BMI270_SCALE_MG,
    .scale_den = S16_RANGE,
    // The newest frame is the latest sample, the others one interval apart before it
    .first_timestamp_us = now_us - ((uint64_t)(frames - 1U) * interval_us),
    .sampling_interval_us = interval_us,
  };

  prv_frame_to_mg(&state->buf[(frames - 1U) * BMI270_FRAME_BYTES], state->last);
  state->stats.fifo_reads++;
  state->stats.samples += frames;

  pbl_accel_report_samples(&s_dev->accel, &batch);
}

// Report what the FIFO holds
static void prv_read_fifo(void) {
  struct pbl_bmi270_state *state = s_dev->state;

  for (unsigned int i = 0U; i < DRAIN_READS_MAX; i++) {
    uint8_t len[2];
    uint16_t frames;
    int ret;

    if (prv_read(BMI270_FIFO_LENGTH_0, len, sizeof(len)) != 0) {
      return;
    }

    frames = bmi270_fifo_frames((uint16_t)(len[0] | ((len[1] & 0x3FU) << 8)), BMI270_SAMPLES_MAX);
    if (frames == 0U) {
      break;
    }

    struct pbl_i2c_msg msgs[] = {
      {.buf = &(uint8_t){BMI270_FIFO_DATA}, .len = 1U, .flags = PBL_I2C_MSG_WRITE},
      {.buf = state->buf,
       .len = frames * BMI270_FRAME_BYTES,
       .flags = PBL_I2C_MSG_READ | PBL_I2C_MSG_DMA_SAFE},
    };

    ret = pbl_i2c_transfer(&s_dev->i2c, msgs, PBL_ARRAY_SIZE(msgs));
    prv_access_done();
    if (ret != 0) {
      state->stats.errors++;
      PBL_LOG_ERR("Could not read the FIFO");
      return;
    }

    prv_report_frames(frames);

    if (frames < BMI270_SAMPLES_MAX) {
      break;
    }
  }

  state->last_service_ms = pbl_uptime_ms();
}

// Out of advanced power save: in it, reads of more than a few dozen frames bring filler bytes
static void prv_drain_fifo(void) {
  const bool aps = s_dev->state->aps;

  if (prv_set_aps(false) == 0) {
    prv_read_fifo();
  }
  if (aps) {
    (void)prv_set_aps(true);
  }
}

static void prv_report_shake(void) {
  struct pbl_bmi270_state *state = s_dev->state;
  uint8_t frame[BMI270_FRAME_BYTES];
  int16_t now[3];
  int8_t direction = 1;
  uint8_t axis;

  if (prv_read(BMI270_DATA_ACC_X, frame, sizeof(frame)) != 0) {
    return;
  }

  prv_frame_to_mg(frame, now);
  axis = bmi270_shake_axis(state->last, now, &direction);
  for (unsigned int i = 0U; i < 3U; i++) {
    state->last[i] = now[i];
  }

  state->stats.shakes++;
  pbl_accel_report_event(&s_dev->accel, &(const struct pbl_accel_event){
                                          .type = PBL_ACCEL_EVENT_SHAKE,
                                          .axis = (IMUCoordinateAxis)axis,
                                          .direction = direction,
                                        });
}

static void prv_service(void) {
  struct pbl_bmi270_state *state = s_dev->state;
  uint8_t status[2];

  // Reading the status releases the latched pin
  if (prv_read(BMI270_INT_STATUS_0, status, sizeof(status)) != 0) {
    return;
  }

  if ((status[1] & BMI270_INT_FFULL) != 0U && state->num_samples > 0U) {
    state->stats.overruns++;
    PBL_LOG_WRN("FIFO full, samples lost");
  }

  if (state->num_samples > 0U && state->odr != 0U) {
    prv_drain_fifo();
  }

  if ((status[0] & BMI270_INT_ANY_MOTION) != 0U && state->shake) {
    prv_report_shake();
  }
}

////////////////////////////////////////////////////////////////////////////////
// Interrupt and watchdog
////////////////////////////////////////////////////////////////////////////////

static uint32_t prv_batch_ms(void) {
  return (prv_interval_us() / 1000U) * s_dev->state->num_samples;
}

static void prv_arm_watchdog(void) {
  struct pbl_bmi270_state *state = s_dev->state;

  if (state->num_samples > 0U && state->odr != 0U) {
    pbl_delayed_work_reschedule(&pbl_sys_workq_high, &state->watchdog,
                                PBL_MSEC(PBL_MAX(2U * prv_batch_ms(), WATCHDOG_MIN_MS)));
  } else {
    pbl_delayed_work_cancel(&state->watchdog);
  }
}

static void prv_irq_work(struct pbl_work *work) {
  pbl_accel_lock(&s_dev->accel);

  prv_service();
  prv_arm_watchdog();

  // A source that came up while the pad was high made no edge
  if (pbl_gpio_get(&s_dev->irq_in)) {
    pbl_work_submit(&pbl_sys_workq_high, &s_dev->state->irq_work);
  }

  pbl_accel_unlock(&s_dev->accel);
}

static void prv_watchdog(struct pbl_work *work) {
  struct pbl_bmi270_state *state = s_dev->state;

  pbl_accel_lock(&s_dev->accel);

  if (state->num_samples > 0U && state->odr != 0U &&
      (pbl_uptime_ms() - state->last_service_ms) >= (3U * prv_batch_ms()) / 2U) {
    state->stats.watchdog++;
    PBL_LOG_WRN("No FIFO interrupt for %" PRIu32 " ms (pad %d), servicing",
                (uint32_t)(pbl_uptime_ms() - state->last_service_ms), pbl_gpio_get(&s_dev->irq_in));
    prv_service();
  }

  prv_arm_watchdog();

  pbl_accel_unlock(&s_dev->accel);
}

static void prv_irq_handler(void *arg) {
  s_dev->state->stats.irqs++;
  pbl_work_submit(&pbl_sys_workq_high, &s_dev->state->irq_work);
}

////////////////////////////////////////////////////////////////////////////////
// Configuration
////////////////////////////////////////////////////////////////////////////////

static uint16_t prv_any_motion_threshold(void) {
  const struct pbl_bmi270_state *state = s_dev->state;

  return bmi270_any_motion_threshold(state->shake_high ? 100U : state->shake_percent,
                                     CONFIG_ACCEL_BMI270_SHAKE_MIN_MG,
                                     CONFIG_ACCEL_BMI270_SHAKE_MAX_MG);
}

// With advanced power save off
static int prv_write_any_motion(bool enable) {
  uint8_t page[16];
  int ret;

  ret = prv_write_byte(BMI270_FEAT_PAGE, BMI270_ANY_MOTION_PAGE);
  if (ret == 0) {
    ret = prv_read(BMI270_FEATURES, page, sizeof(page));
  }
  if (ret != 0) {
    return ret;
  }

  bmi270_any_motion_encode(CONFIG_ACCEL_BMI270_SHAKE_DURATION, prv_any_motion_threshold(), enable,
                           &page[BMI270_ANY_MOTION_OFFSET]);

  return prv_write(BMI270_FEATURES, page, sizeof(page));
}

// Set the part up for what is asked of it, from scratch
static int prv_apply(void) {
  struct pbl_bmi270_state *state = s_dev->state;
  const bool streaming = state->num_samples > 0U;
  const struct bmi270_mode mode = bmi270_mode_for(streaming ? state->fifo_odr : 0U, state->shake);
  const uint16_t wtm = streaming ? (uint16_t)(state->num_samples * BMI270_FRAME_BYTES) : 0U;
  const uint8_t map_data =
      streaming ? ((s_dev->int_pin == 2U) ? BMI270_MAP_FWM_INT2 : BMI270_MAP_FWM_INT1) : 0U;
  int ret;

  ret = prv_set_aps(false);

  // Interrupts off while the rest changes
  if (ret == 0) {
    ret = prv_write_byte(BMI270_INT_MAP_DATA, 0U);
  }
  if (ret == 0) {
    ret = prv_write_byte(prv_pin_reg(BMI270_INT1_MAP_FEAT, BMI270_INT2_MAP_FEAT), 0U);
  }

  if (ret == 0 && mode.odr != 0U) {
    ret = prv_write_byte(BMI270_ACC_CONF,
                         BMI270_ACC_CONF_VAL(mode.odr, CONFIG_ACCEL_BMI270_AVG_LOG2));
    if (ret == 0) {
      ret = prv_write_byte(BMI270_FIFO_DOWNS, BMI270_FIFO_DOWNS_VAL(mode.downs));
    }
    if (ret == 0) {
      ret = prv_write(BMI270_FIFO_WTM_0, (const uint8_t[]){(uint8_t)wtm, (uint8_t)(wtm >> 8)}, 2U);
    }
  }

  if (ret == 0) {
    ret = prv_write_byte(BMI270_FIFO_CONFIG_1, streaming ? BMI270_FIFO_CONFIG_1_ACC_EN : 0U);
  }
  if (ret == 0) {
    ret = prv_write_any_motion(state->shake);
  }
  if (ret == 0) {
    ret = prv_write_byte(BMI270_PWR_CTRL, (mode.odr != 0U) ? BMI270_PWR_CTRL_ACC_EN : 0U);
  }
  if (ret == 0) {
    ret = prv_write_byte(BMI270_CMD, BMI270_CMD_FIFO_FLUSH);
  }
  if (ret == 0 && mode.odr != 0U) {
    ret = prv_write_byte(BMI270_INT_MAP_DATA, map_data);
  }
  if (ret == 0 && state->shake) {
    ret = prv_write_byte(prv_pin_reg(BMI270_INT1_MAP_FEAT, BMI270_INT2_MAP_FEAT),
                         BMI270_INT_ANY_MOTION);
  }
  if (ret == 0) {
    ret = prv_set_aps(true);
  }

  if (ret != 0) {
    PBL_LOG_ERR("Could not configure (%d)", ret);
    return -EIO;
  }

  state->odr = mode.odr;
  state->downs = mode.downs;
  state->last_service_ms = pbl_uptime_ms();
  prv_arm_watchdog();

  PBL_LOG_DBG("ODR %" PRIu32 " us, FIFO every %" PRIu32 " us by %u, shake %d",
              (uint32_t)(mode.odr != 0U ? BMI270_ODR_US(mode.odr) : 0U), prv_interval_us(),
              state->num_samples, state->shake);

  // The pad may have latched before its interrupt was mapped again
  if (pbl_gpio_get(&s_dev->irq_in)) {
    pbl_work_submit(&pbl_sys_workq_high, &state->irq_work);
  }

  return 0;
}

// Report what the FIFO holds before it is changed
static int prv_reconfigure(void) {
  if (s_dev->state->num_samples > 0U && s_dev->state->odr != 0U) {
    prv_drain_fifo();
  }

  return prv_apply();
}

static int prv_load_config_file(void) {
  uint8_t status = 0U;
  int ret;

  ret = prv_set_aps(false);
  if (ret == 0) {
    ret = prv_write_byte(BMI270_INIT_CTRL, 0U);
  }

  for (uint32_t off = 0U; ret == 0 && off < BMI270_CONFIG_FILE_SIZE; off += CONFIG_CHUNK_BYTES) {
    const uint16_t word = (uint16_t)(off / 2U);

    ret = prv_write(BMI270_INIT_ADDR_0, (const uint8_t[]){word & 0x0FU, (uint8_t)(word >> 4)}, 2U);
    if (ret == 0) {
      ret = prv_write(BMI270_INIT_DATA, &bmi270_config_file[off], CONFIG_CHUNK_BYTES);
    }
  }

  if (ret == 0) {
    ret = prv_write_byte(BMI270_INIT_CTRL, 1U);
  }
  if (ret != 0) {
    return ret;
  }

  ret = PBL_POLL_SLEEP_MS(
      prv_read(BMI270_INTERNAL_STATUS, &status, 1U) == 0 &&
          (status & BMI270_INTERNAL_STATUS_MSG_MASK) == BMI270_INTERNAL_STATUS_INIT_OK,
      INIT_POLL_MS, INIT_TIMEOUT_MS);
  if (ret != 0) {
    PBL_LOG_ERR("Configuration file not taken (INTERNAL_STATUS 0x%02x)", status);
    return -EIO;
  }

  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Accelerometer interface
////////////////////////////////////////////////////////////////////////////////

static int prv_init(const struct pbl_accel *accel) {
  struct pbl_bmi270_state *state;
  uint8_t id = 0U;
  int ret;

  s_dev = PBL_CONTAINER_OF(accel, const struct pbl_bmi270, accel);
  state = s_dev->state;
  state->fifo_odr = bmi270_odr_for_interval(40000U);
  state->shake_percent = 50U;
  pbl_work_init(&state->irq_work, prv_irq_work);
  pbl_delayed_work_init(&state->watchdog, prv_watchdog);

  if (prv_read(BMI270_CHIP_ID, &id, 1U) != 0 || id != BMI270_CHIP_ID_VAL) {
    PBL_LOG_ERR("Unexpected chip ID 0x%02x", id);
    return -ENODEV;
  }

  // From reset, in advanced power save
  state->aps = false;
  ret = prv_write_byte(BMI270_CMD, BMI270_CMD_SOFTRESET);
  if (ret != 0) {
    return -EIO;
  }
  pbl_sleep_ms(SOFTRESET_MS);
  state->aps = true;

  ret = prv_load_config_file();
  if (ret != 0) {
    return ret;
  }

  // Interrupt pin: push-pull, active high, latched
  ret = prv_write_byte(prv_pin_reg(BMI270_INT1_IO_CTRL, BMI270_INT2_IO_CTRL),
                       BMI270_IO_CTRL_OUTPUT_EN | BMI270_IO_CTRL_LVL_HIGH);
  if (ret == 0) {
    ret = prv_write_byte(BMI270_INT_LATCH, BMI270_INT_LATCH_ON);
  }
  if (ret == 0) {
    ret = prv_write_byte(BMI270_ACC_RANGE, ACC_RANGE);
  }
  // Stream mode, no sensor time
  if (ret == 0) {
    ret = prv_write_byte(BMI270_FIFO_CONFIG_0, 0U);
  }
  if (ret != 0) {
    return -EIO;
  }

  ret = pbl_device_init(&s_dev->irq.intc->dev);
  if (ret == 0) {
    ret = pbl_gpio_configure(&s_dev->irq_in, PBL_GPIO_INPUT);
  }
  if (ret == 0) {
    ret = pbl_intc_request(&s_dev->irq, prv_irq_handler, nullptr);
  }
  if (ret == 0) {
    ret = pbl_intc_enable(&s_dev->irq);
  }
  if (ret != 0) {
    PBL_LOG_ERR("Could not set up the interrupt (%d)", ret);
    return ret;
  }

  ret = prv_apply();
  if (ret != 0) {
    return ret;
  }

  PBL_LOG_INFO("BMI270 at 0x%02x, interrupt on INT%u", s_dev->i2c.addr, s_dev->int_pin);

  return 0;
}

static int prv_set_rotated(const struct pbl_accel *accel, bool rotated) {
  s_dev->state->rotated = rotated;

  return 0;
}

static int prv_set_sampling_interval(const struct pbl_accel *accel, uint32_t interval_us,
                                     uint32_t *actual_us) {
  struct pbl_bmi270_state *state = s_dev->state;
  const uint8_t odr = bmi270_odr_for_interval(interval_us);
  int ret = 0;

  if (odr != state->fifo_odr) {
    if (state->num_samples > 0U && state->odr != 0U) {
      prv_drain_fifo();
    }
    state->fifo_odr = odr;
    ret = prv_apply();
  }

  *actual_us = prv_interval_us();

  return ret;
}

static uint32_t prv_get_sampling_interval(const struct pbl_accel *accel) {
  return prv_interval_us();
}

static uint32_t prv_get_max_num_samples(const struct pbl_accel *accel) {
  return BMI270_SAMPLES_MAX;
}

static int prv_set_num_samples(const struct pbl_accel *accel, uint32_t num_samples) {
  struct pbl_bmi270_state *state = s_dev->state;

  num_samples = PBL_MIN(num_samples, (uint32_t)BMI270_SAMPLES_MAX);
  if (num_samples == state->num_samples) {
    return 0;
  }

  if (state->num_samples > 0U && state->odr != 0U) {
    prv_drain_fifo();
  }
  state->num_samples = (uint16_t)num_samples;

  return prv_apply();
}

static bool prv_data_ready(int *ret) {
  uint8_t status = 0U;

  *ret = prv_read(BMI270_STATUS, &status, 1U);

  return *ret != 0 || (status & 0x80U) != 0U;
}

static int prv_peek(const struct pbl_accel *accel, struct pbl_accel_sample *sample) {
  struct pbl_bmi270_state *state = s_dev->state;
  uint8_t frame[BMI270_FRAME_BYTES];
  int16_t mg[3];
  int ret;

  if (state->odr == 0U) {
    // One sample at 100 Hz, then off again
    int rd = 0;

    ret = prv_set_aps(false);
    if (ret == 0) {
      ret = prv_write_byte(BMI270_ACC_CONF,
                           BMI270_ACC_CONF_VAL(BMI270_ODR_100HZ, CONFIG_ACCEL_BMI270_AVG_LOG2));
    }
    if (ret == 0) {
      ret = prv_write_byte(BMI270_PWR_CTRL, BMI270_PWR_CTRL_ACC_EN);
    }
    if (ret == 0) {
      ret = PBL_POLL_SLEEP_MS(prv_data_ready(&rd), PEEK_POLL_MS, PEEK_TIMEOUT_MS);
      ret = (rd != 0) ? rd : ret;
    }
    if (ret == 0) {
      ret = prv_read(BMI270_DATA_ACC_X, frame, sizeof(frame));
    }
    (void)prv_write_byte(BMI270_PWR_CTRL, 0U);
    (void)prv_set_aps(true);
  } else {
    // The data registers follow the ODR, whatever the FIFO holds
    ret = prv_read(BMI270_DATA_ACC_X, frame, sizeof(frame));
  }

  if (ret != 0) {
    return -EIO;
  }

  prv_frame_to_mg(frame, mg);
  sample->x = mg[AXIS_X];
  sample->y = mg[AXIS_Y];
  sample->z = mg[AXIS_Z];
  sample->timestamp_us = pbl_realtime_ms() * 1000ULL;

  return 0;
}

static int prv_set_shake_detection(const struct pbl_accel *accel, bool on) {
  if (s_dev->state->shake == on) {
    return 0;
  }

  s_dev->state->shake = on;

  return prv_reconfigure();
}

static int prv_set_shake_sensitivity_high(const struct pbl_accel *accel, bool high) {
  s_dev->state->shake_high = high;

  return s_dev->state->shake ? prv_reconfigure() : 0;
}

static int prv_set_shake_sensitivity_percent(const struct pbl_accel *accel, uint8_t percent) {
  s_dev->state->shake_percent = PBL_MIN(percent, 100U);

  return (s_dev->state->shake && !s_dev->state->shake_high) ? prv_reconfigure() : 0;
}

const struct pbl_accel_ops pbl_bmi270_ops = {
  .init = prv_init,
  .set_rotated = prv_set_rotated,
  .set_sampling_interval = prv_set_sampling_interval,
  .get_sampling_interval = prv_get_sampling_interval,
  .set_num_samples = prv_set_num_samples,
  .get_max_num_samples = prv_get_max_num_samples,
  .peek = prv_peek,
  .set_shake_detection = prv_set_shake_detection,
  .set_shake_sensitivity_high = prv_set_shake_sensitivity_high,
  .set_shake_sensitivity_percent = prv_set_shake_sensitivity_percent,
};

void pbl_bmi270_get_stats(const struct pbl_accel *accel, struct pbl_bmi270_stats *stats) {
  pbl_accel_lock(accel);
  *stats = PBL_CONTAINER_OF(accel, const struct pbl_bmi270, accel)->state->stats;
  pbl_accel_unlock(accel);
}

#include PBL_DT_INSTANCES(bosch_bmi270)

#ifdef CONFIG_IMU_SHELL
static int prv_cmd_stats(const struct pbl_shell *sh, size_t argc, char **argv) {
  const struct pbl_bmi270_state *state;
  struct pbl_bmi270_stats stats;

  if (s_dev == nullptr || !pbl_device_is_ready(&s_dev->accel.dev)) {
    pbl_shell_error(sh, "not ready");
    return -ENODEV;
  }

  state = s_dev->state;
  pbl_bmi270_get_stats(&s_dev->accel, &stats);
  pbl_shell_print(sh,
                  "ODR %" PRIu32 " us, FIFO every %" PRIu32
                  " us (down-sampled by %u), "
                  "batch %u, shake %s",
                  (uint32_t)(state->odr != 0U ? BMI270_ODR_US(state->odr) : 0U), prv_interval_us(),
                  1U << state->downs, state->num_samples,
                  state->shake ? (state->shake_high ? "on, high" : "on") : "off");
  pbl_shell_print(sh,
                  "irqs %" PRIu32 " fifo reads %" PRIu32 " samples %" PRIu32 " overruns %" PRIu32
                  " shakes %" PRIu32 " watchdog %" PRIu32 " errors %" PRIu32 " pad %d",
                  stats.irqs, stats.fifo_reads, stats.samples, stats.overruns, stats.shakes,
                  stats.watchdog, stats.errors, pbl_gpio_get(&s_dev->irq_in));

  return 0;
}

PBL_SHELL_CMD_REGISTER(bmi270, nullptr, "BMI270 driver statistics", prv_cmd_stats);
#endif
