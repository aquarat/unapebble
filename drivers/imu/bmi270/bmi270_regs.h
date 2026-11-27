/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// BMI270 registers and the driver's choices of configuration, apart from the bus so the unit tests
// can check them. Reference: BMI270 data sheet (BST-BMI270-DS000-08), and Bosch Sensortec's
// BMI270 SensorAPI v2.86.1 for the feature engine's layout.

#include <stdbool.h>
#include <stdint.h>

#define BMI270_CHIP_ID         0x00U
#define BMI270_ERR_REG         0x02U
#define BMI270_STATUS          0x03U
#define BMI270_DATA_ACC_X      0x0CU
#define BMI270_INT_STATUS_0    0x1CU
#define BMI270_INT_STATUS_1    0x1DU
#define BMI270_INTERNAL_STATUS 0x21U
#define BMI270_FIFO_LENGTH_0   0x24U
#define BMI270_FIFO_DATA       0x26U
#define BMI270_FEAT_PAGE       0x2FU
#define BMI270_FEATURES        0x30U
#define BMI270_ACC_CONF        0x40U
#define BMI270_ACC_RANGE       0x41U
#define BMI270_FIFO_DOWNS      0x45U
#define BMI270_FIFO_WTM_0      0x46U
#define BMI270_FIFO_CONFIG_0   0x48U
#define BMI270_FIFO_CONFIG_1   0x49U
#define BMI270_INT1_IO_CTRL    0x53U
#define BMI270_INT2_IO_CTRL    0x54U
#define BMI270_INT_LATCH       0x55U
#define BMI270_INT1_MAP_FEAT   0x56U
#define BMI270_INT2_MAP_FEAT   0x57U
#define BMI270_INT_MAP_DATA    0x58U
#define BMI270_INIT_CTRL       0x59U
#define BMI270_INIT_ADDR_0     0x5BU
#define BMI270_INIT_DATA       0x5EU
#define BMI270_PWR_CONF        0x7CU
#define BMI270_PWR_CTRL        0x7DU
#define BMI270_CMD             0x7EU

#define BMI270_CHIP_ID_VAL 0x24U

#define BMI270_CMD_SOFTRESET  0xB6U
#define BMI270_CMD_FIFO_FLUSH 0xB0U

// INTERNAL_STATUS: the message once the configuration file is running
#define BMI270_INTERNAL_STATUS_MSG_MASK 0x0FU
#define BMI270_INTERNAL_STATUS_INIT_OK  0x01U

// INT_STATUS_0, the feature engine's interrupts, and their bits in INTx_MAP_FEAT
#define BMI270_INT_ANY_MOTION (1U << 6)
// INT_STATUS_1, the data interrupts
#define BMI270_INT_FFULL (1U << 0)
#define BMI270_INT_FWM   (1U << 1)

// INT_MAP_DATA: FIFO watermark to INT1, INT2
#define BMI270_MAP_FWM_INT1 (1U << 1)
#define BMI270_MAP_FWM_INT2 (1U << 5)

// INTx_IO_CTRL: active high, push-pull, output on
#define BMI270_IO_CTRL_LVL_HIGH  (1U << 1)
#define BMI270_IO_CTRL_OD        (1U << 2)
#define BMI270_IO_CTRL_OUTPUT_EN (1U << 3)

// INT_LATCH: interrupts held until their status is read
#define BMI270_INT_LATCH_ON 0x01U

// ACC_CONF: ODR in bits 3:0, averaging (low power) or bandwidth in 6:4, performance mode in 7
#define BMI270_ACC_CONF_VAL(odr, bwp) ((uint8_t)(((bwp) << 4) | (odr)))

// ACC_RANGE
#define BMI270_ACC_RANGE_2G  0x00U
#define BMI270_ACC_RANGE_4G  0x01U
#define BMI270_ACC_RANGE_8G  0x02U
#define BMI270_ACC_RANGE_16G 0x03U

// FIFO_DOWNS: the accelerometer's frames down-sampled by 2^n, from filtered data
#define BMI270_FIFO_DOWNS_VAL(n) ((uint8_t)(0x80U | ((n) << 4)))

// FIFO_CONFIG_1: accelerometer frames only, without headers
#define BMI270_FIFO_CONFIG_1_ACC_EN 0x40U

// PWR_CONF: advanced power save, and the FIFO readable in it
#define BMI270_PWR_CONF_ADV_POWER_SAVE 0x01U
#define BMI270_PWR_CONF_FIFO_SELF_WAKE 0x02U

// PWR_CTRL
#define BMI270_PWR_CTRL_ACC_EN 0x04U

// The FIFO, 2 KiB, and a headerless accelerometer frame: X, Y, Z, little endian
#define BMI270_FIFO_BYTES  2048U
#define BMI270_FRAME_BYTES 6U

#define BMI270_CONFIG_FILE_SIZE 8192U

// Feature engine: any-motion is at offset 0x0C of page 1, two words: the duration (13 bits, in
// 20 ms samples of its 50 Hz) with the axes enabled in bits 13 to 15, then the threshold (11 bits,
// 1/2048 g) with the enable in bit 15
#define BMI270_ANY_MOTION_PAGE    1U
#define BMI270_ANY_MOTION_OFFSET  0x0CU
#define BMI270_ANY_MOTION_XYZ     0xE000U
#define BMI270_ANY_MOTION_EN      0x8000U
#define BMI270_ANY_MOTION_DUR_MAX 0x1FFFU
#define BMI270_ANY_MOTION_THR_MAX 0x07FFU

// The feature engine runs on 50 Hz data, which it needs at least
#define BMI270_FEATURE_ODR_US 20000U

// Output data rates, 1600 Hz / 2^(12 - odr): 12.5 Hz (code 5) to 100 Hz (code 8), the range the
// accelerometer service asks for
#define BMI270_ODR_12_5HZ  0x05U
#define BMI270_ODR_100HZ   0x08U
#define BMI270_ODR_US(odr) (625U << (12U - (odr)))

extern const uint8_t bmi270_config_file[BMI270_CONFIG_FILE_SIZE];

//! The longest interval the part samples at that is no longer than @p interval_us, saturating at
//! 100 Hz and 12.5 Hz, as its ODR code.
static inline uint8_t bmi270_odr_for_interval(uint32_t interval_us) {
  uint8_t odr = BMI270_ODR_12_5HZ;

  while (odr < BMI270_ODR_100HZ && BMI270_ODR_US(odr) > interval_us) {
    odr++;
  }

  return odr;
}

//! How the part runs for what is asked of it.
struct bmi270_mode {
  //! Accelerometer ODR code, 0 when off.
  uint8_t odr;
  //! FIFO down-sampling, as a power of 2.
  uint8_t downs;
};

//! The mode for a FIFO sampling at @p fifo_odr (0 for none) and shake detection, which needs
//! 50 Hz: the FIFO then down-samples to its own rate.
static inline struct bmi270_mode bmi270_mode_for(uint8_t fifo_odr, bool shake) {
  const uint8_t feature_odr = bmi270_odr_for_interval(BMI270_FEATURE_ODR_US);
  struct bmi270_mode mode = {.odr = fifo_odr, .downs = 0U};

  if (shake && (fifo_odr == 0U || fifo_odr < feature_odr)) {
    mode.odr = feature_odr;
    mode.downs = (fifo_odr == 0U) ? 0U : (uint8_t)(feature_odr - fifo_odr);
  }

  return mode;
}

//! Any-motion threshold, in 1/2048 g, for a shake sensitivity of @p percent (0 the least
//! sensitive, at @p max_mg, 100 the most, at @p min_mg).
static inline uint16_t bmi270_any_motion_threshold(uint8_t percent, uint16_t min_mg,
                                                   uint16_t max_mg) {
  uint32_t mg;

  if (percent > 100U) {
    percent = 100U;
  }

  mg = max_mg - (((uint32_t)(max_mg - min_mg) * percent) / 100U);
  mg = (mg * 2048U + 500U) / 1000U;

  return (uint16_t)((mg > BMI270_ANY_MOTION_THR_MAX) ? BMI270_ANY_MOTION_THR_MAX : mg);
}

//! The any-motion words, little endian, for page 1 at its offset.
static inline void bmi270_any_motion_encode(uint16_t duration, uint16_t threshold, bool enable,
                                            uint8_t out[4]) {
  uint16_t w0 = (uint16_t)(BMI270_ANY_MOTION_XYZ | (duration & BMI270_ANY_MOTION_DUR_MAX));
  uint16_t w1 =
      (uint16_t)((threshold & BMI270_ANY_MOTION_THR_MAX) | (enable ? BMI270_ANY_MOTION_EN : 0U));

  out[0] = (uint8_t)w0;
  out[1] = (uint8_t)(w0 >> 8);
  out[2] = (uint8_t)w1;
  out[3] = (uint8_t)(w1 >> 8);
}

//! Complete frames in a FIFO fill level of @p bytes, at most @p max.
static inline uint16_t bmi270_fifo_frames(uint16_t bytes, uint16_t max) {
  uint16_t frames = (uint16_t)(bytes / BMI270_FRAME_BYTES);

  return (frames > max) ? max : frames;
}

//! The watch axis, 0 to 2, that moved most from @p before to @p after, with its direction.
static inline uint8_t bmi270_shake_axis(const int16_t before[3], const int16_t after[3],
                                        int8_t *direction) {
  uint8_t axis = 2U;
  int32_t best = -1;

  for (uint8_t i = 0U; i < 3U; i++) {
    int32_t d = (int32_t)after[i] - (int32_t)before[i];
    int32_t mag = (d < 0) ? -d : d;

    if (mag > best) {
      best = mag;
      axis = i;
      *direction = (d < 0) ? -1 : 1;
    }
  }

  return axis;
}
