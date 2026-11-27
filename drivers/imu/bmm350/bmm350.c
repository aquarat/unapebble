/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "bmm350_regs.h"

#include <errno.h>
#include <inttypes.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/i2c.h>
#include <pbl/drivers/imu/bmm350/bmm350.h>
#include <pbl/drivers/mag.h>
#include <pbl/kernel/delay.h>
#include <pbl/logging/logging.h>
#include <pbl/services/imu/units.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

PBL_LOG_MODULE_DECLARE(driver_mag, CONFIG_DRIVER_IMU_LOG_LEVEL);

// Implementation notes:
//
// - Suspended unless sampling. Sampling starts with a magnetic reset (BR then FGR), as Bosch's
//   SensorAPI does at init, then runs the part in its normal mode at the ODR for the interval
//   asked, averaging CONFIG_MAG_BMM350_AVG_LOG2 samples as far as that ODR allows.
// - A delayed work item reads the data registers at the ODR; the interrupt line, PC2 on the UNA
//   Watch, is not used.

// The one instance, for the work item
static const struct pbl_bmm350 *s_dev;

static int prv_read(uint8_t reg, uint8_t *data, uint16_t len) {
  uint8_t buf[BMM350_DUMMY_BYTES + BMM350_DATA_BYTES];
  int ret;

  if (len > BMM350_DATA_BYTES) {
    return -EINVAL;
  }

  ret = pbl_i2c_burst_read(&s_dev->i2c, reg, buf, BMM350_DUMMY_BYTES + len);
  if (ret != 0) {
    s_dev->state->errors++;
    PBL_LOG_ERR("Could not read register 0x%02x (%d)", reg, ret);
    return ret;
  }

  for (uint16_t i = 0U; i < len; i++) {
    data[i] = buf[BMM350_DUMMY_BYTES + i];
  }

  return 0;
}

static int prv_write(uint8_t reg, uint8_t val) {
  int ret = pbl_i2c_reg_write_byte(&s_dev->i2c, reg, val);

  if (ret != 0) {
    s_dev->state->errors++;
    PBL_LOG_ERR("Could not write register 0x%02x (%d)", reg, ret);
  }

  return ret;
}

static int prv_command(uint8_t cmd, uint32_t wait_us) {
  int ret = prv_write(BMM350_PMU_CMD, cmd);

  if (ret == 0) {
    pbl_sleep_us(wait_us);
  }

  return ret;
}

static int prv_read_otp(uint16_t otp[BMM350_OTP_WORDS]) {
  for (uint8_t addr = 0U; addr < BMM350_OTP_WORDS; addr++) {
    uint8_t status = 0U;
    uint8_t word[2];
    unsigned int polls = 0U;
    int ret;

    ret = prv_write(BMM350_OTP_CMD, BMM350_OTP_CMD_DIR_READ | addr);
    while (ret == 0 && (status & BMM350_OTP_STATUS_DONE) == 0U) {
      if (++polls > BMM350_OTP_POLLS) {
        ret = -ETIMEDOUT;
        break;
      }
      pbl_sleep_us(BMM350_OTP_POLL_US);
      ret = prv_read(BMM350_OTP_STATUS, &status, 1U);
      if (ret == 0 && (status & BMM350_OTP_STATUS_ERROR) != 0U) {
        ret = -EIO;
      }
    }

    // MSB then LSB
    if (ret == 0) {
      ret = prv_read(BMM350_OTP_DATA_MSB, word, sizeof(word));
    }
    if (ret != 0) {
      PBL_LOG_ERR("Could not read OTP word %u (%d, status 0x%02x)", addr, ret, status);
      return ret;
    }

    otp[addr] = (uint16_t)((word[0] << 8) | word[1]);
  }

  return 0;
}

// From suspend: a reset of the sensing elements, then the ODR
static int prv_start(uint8_t odr) {
  uint8_t status = 0U;
  int ret;

  ret = prv_command(BMM350_PMU_CMD_BR, BMM350_BR_US);
  if (ret == 0) {
    ret = prv_read(BMM350_PMU_CMD_STATUS_0, &status, 1U);
  }
  if (ret == 0 && BMM350_PMU_STATUS_CMD(status) != BMM350_PMU_CMD_BR) {
    ret = -EIO;
  }
  if (ret == 0) {
    ret = prv_command(BMM350_PMU_CMD_FGR, BMM350_FGR_US);
  }
  if (ret == 0) {
    ret = prv_read(BMM350_PMU_CMD_STATUS_0, &status, 1U);
  }
  if (ret == 0 && BMM350_PMU_STATUS_CMD(status) != BMM350_PMU_CMD_FGR) {
    ret = -EIO;
  }
  if (ret != 0) {
    PBL_LOG_ERR("Magnetic reset failed (%d, status 0x%02x)", ret, status);
    return ret;
  }

  ret = prv_write(BMM350_PMU_CMD_AGGR_SET,
                  (uint8_t)((bmm350_avg_for(odr, CONFIG_MAG_BMM350_AVG_LOG2) << 4) | odr));
  if (ret == 0) {
    ret = prv_command(BMM350_PMU_CMD_UPD_OAE, BMM350_UPD_OAE_US);
  }
  if (ret == 0) {
    ret = prv_command(BMM350_PMU_CMD_NM, BMM350_SUSPEND_TO_NORMAL_US);
  }

  return ret;
}

static int prv_suspend(void) {
  return prv_command(BMM350_PMU_CMD_SUS, BMM350_GOTO_SUSPEND_US);
}

static int prv_read_sample(MagData *sample) {
  struct pbl_bmm350_state *state = s_dev->state;
  uint8_t data[BMM350_DATA_BYTES];
  int32_t raw[4];
  float out[4];
  int16_t mg[3];
  int ret;

  ret = prv_read(BMM350_MAG_X_XLSB, data, sizeof(data));
  if (ret != 0) {
    return ret;
  }

  bmm350_unpack(data, raw);
  bmm350_compensate(&state->comp, raw, out);

  // Microtesla to milligauss, on the watch's axes
  for (IMUCoordinateAxis axis = AXIS_X; axis <= AXIS_Z; axis++) {
    float v = out[s_dev->axis_map[axis]] * 10.0f * (float)s_dev->axis_dir[axis];

    if (state->rotated && (axis == AXIS_X || axis == AXIS_Y)) {
      v = -v;
    }
    mg[axis] =
        (int16_t)PBL_CLAMP(v + ((v < 0.0f) ? -0.5f : 0.5f), (float)INT16_MIN, (float)INT16_MAX);
  }

  sample->x = mg[AXIS_X];
  sample->y = mg[AXIS_Y];
  sample->z = mg[AXIS_Z];
  state->last = *sample;
  state->temp_cdeg = (int16_t)(out[3] * 100.0f);
  state->have_sample = true;

  return 0;
}

static void prv_poll(struct pbl_work *work) {
  struct pbl_bmm350_state *state = s_dev->state;
  MagData sample;

  pbl_mag_lock(&s_dev->mag);

  if (state->odr != 0U) {
    if (prv_read_sample(&sample) == 0) {
      pbl_mag_report_sample(&s_dev->mag, &sample);
    }

    // Sampling changes hold the lock too
    if (!pbl_delayed_work_is_pending(&state->poll)) {
      pbl_delayed_work_schedule_next(&state->poll,
                                     pbl_ms_to_ticks(BMM350_ODR_US(state->odr) / 1000U));
    }
  }

  pbl_mag_unlock(&s_dev->mag);
}

static int prv_init(const struct pbl_mag *mag) {
  uint16_t otp[BMM350_OTP_WORDS];
  uint8_t id = 0U;
  int ret;

  s_dev = PBL_CONTAINER_OF(mag, const struct pbl_bmm350, mag);
  pbl_delayed_work_init(&s_dev->state->poll, prv_poll);

  pbl_sleep_us(BMM350_STARTUP_US);
  if (prv_read(BMM350_CHIP_ID, &id, 1U) != 0 || id != BMM350_CHIP_ID_VAL) {
    PBL_LOG_ERR("Unexpected chip ID 0x%02x", id);
    return -ENODEV;
  }

  ret = prv_write(BMM350_CMD, BMM350_CMD_SOFTRESET);
  if (ret != 0) {
    return -EIO;
  }
  pbl_sleep_us(BMM350_SOFTRESET_US);

  ret = prv_read_otp(otp);
  if (ret == 0) {
    ret = prv_write(BMM350_OTP_CMD, BMM350_OTP_CMD_PWR_OFF);
  }
  if (ret == 0) {
    ret = prv_write(BMM350_PMU_CMD_AXIS_EN, BMM350_AXIS_EN_XYZ);
  }
  if (ret != 0) {
    return -EIO;
  }

  bmm350_parse_otp(otp, &s_dev->state->comp);

  PBL_LOG_INFO("BMM350 at 0x%02x, calibration from 23 + %d/512 C", s_dev->i2c.addr,
               (int)bmm350_sign_extend(otp[0x18], 16U));

  return 0;
}

static int prv_set_rotated(const struct pbl_mag *mag, bool rotated) {
  s_dev->state->rotated = rotated;

  return 0;
}

static int prv_set_sampling_interval(const struct pbl_mag *mag, uint32_t interval_us,
                                     uint32_t *actual_us) {
  struct pbl_bmm350_state *state = s_dev->state;
  const uint8_t odr = (interval_us == 0U) ? 0U : bmm350_odr_for_interval(interval_us);
  int ret = 0;

  if (odr != state->odr) {
    PBL_LOG_DBG("Sampling every %" PRIu32 " us", (uint32_t)(odr == 0U ? 0U : BMM350_ODR_US(odr)));

    if (state->odr != 0U) {
      pbl_delayed_work_cancel(&state->poll);
      ret = prv_suspend();
      state->odr = 0U;
    }
    if (ret == 0 && odr != 0U) {
      state->have_sample = false;
      ret = prv_start(odr);
      if (ret == 0) {
        state->odr = odr;
        (void)pbl_delayed_work_reschedule(&pbl_sys_workq_high, &state->poll,
                                          PBL_TICKS(pbl_ms_to_ticks(BMM350_ODR_US(odr) / 1000U)));
      } else {
        (void)prv_suspend();
      }
    }
    if (ret != 0) {
      return -EIO;
    }
  }

  *actual_us = (state->odr == 0U) ? 0U : BMM350_ODR_US(state->odr);

  return 0;
}

static int prv_peek(const struct pbl_mag *mag, MagData *sample) {
  if (s_dev->state->odr == 0U) {
    return -ENODATA;
  }

  return (prv_read_sample(sample) == 0) ? 0 : -EIO;
}

const struct pbl_mag_ops pbl_bmm350_ops = {
  .init = prv_init,
  .set_rotated = prv_set_rotated,
  .set_sampling_interval = prv_set_sampling_interval,
  .peek = prv_peek,
};

#include PBL_DT_INSTANCES(bosch_bmm350)
