/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// MS5837-02BA commands, the check of its calibration PROM and the compensation of its readings,
// apart from the bus so the unit tests can check them. Reference: TE Connectivity MS5837-02BA01
// data sheet; the stock UNA Watch firmware (MS5837.cpp) does the same.

#include <stdbool.h>
#include <stdint.h>

#define MS5837_CMD_RESET    0x1EU
#define MS5837_CMD_ADC_READ 0x00U
#define MS5837_CMD_PROM(i)  ((uint8_t)(0xA0U + ((i) << 1)))
// A conversion of D1 (pressure) or D2 (temperature) at OSR 256 << osr, osr 0 to 5
#define MS5837_CMD_D1(osr) ((uint8_t)(0x40U + ((osr) << 1)))
#define MS5837_CMD_D2(osr) ((uint8_t)(0x50U + ((osr) << 1)))

#define MS5837_OSR_MAX 5U

// The PROM's seven words: C0, the CRC and the version, then C1 to C6
#define MS5837_PROM_WORDS 7U

#define MS5837_RESET_MS 10U

// The 02BA variants' version codes, C0 bits 11:5, which share the 02BA compensation
#define MS5837_VERSION_02BA01 0x00U
#define MS5837_VERSION_02BA21 0x15U

//! The longest a conversion takes at @p osr, in microseconds.
static inline uint32_t ms5837_conversion_us(uint8_t osr) {
  static const uint16_t us[MS5837_OSR_MAX + 1U] = {600U, 1170U, 2280U, 4540U, 9040U, 18080U};

  return us[(osr > MS5837_OSR_MAX) ? MS5837_OSR_MAX : osr];
}

//! The 4-bit CRC of the PROM, as stored in the top of C0 (AN520).
static inline uint8_t ms5837_crc4(const uint16_t prom[MS5837_PROM_WORDS]) {
  uint16_t rem = 0U;

  for (unsigned int i = 0U; i < 16U; i++) {
    // C0 without its CRC, and an eighth word of zero
    uint16_t word = (i >> 1) < MS5837_PROM_WORDS ? prom[i >> 1] : 0U;

    if ((i >> 1) == 0U) {
      word &= 0x0FFFU;
    }
    rem ^= (i & 1U) ? (word & 0x00FFU) : (word >> 8);
    for (unsigned int b = 0U; b < 8U; b++) {
      rem = (rem & 0x8000U) ? (uint16_t)((rem << 1) ^ 0x3000U) : (uint16_t)(rem << 1);
    }
  }

  return (uint8_t)((rem >> 12) & 0x0FU);
}

//! The version code of the part, C0 bits 11:5.
static inline uint8_t ms5837_version(const uint16_t prom[MS5837_PROM_WORDS]) {
  return (uint8_t)((prom[0] >> 5) & 0x7FU);
}

//! Whether the PROM is a 02BA's that its CRC vouches for. An all-zero PROM passes the CRC, and is
//! no part's.
static inline bool ms5837_prom_valid(const uint16_t prom[MS5837_PROM_WORDS]) {
  bool zero = true;

  for (unsigned int i = 1U; i < MS5837_PROM_WORDS; i++) {
    zero = zero && prom[i] == 0U;
  }

  return !zero && ms5837_crc4(prom) == (prom[0] >> 12) &&
         (ms5837_version(prom) == MS5837_VERSION_02BA01 ||
          ms5837_version(prom) == MS5837_VERSION_02BA21);
}

//! Pressure in pascals, and temperature in hundredths of a degree C, from the raw D1 and D2: the
//! 02BA's first and second order compensation, in which 0.01 mbar is 1 Pa.
static inline void ms5837_compensate(const uint16_t prom[MS5837_PROM_WORDS], uint32_t d1,
                                     uint32_t d2, int32_t *pa, int32_t *temp_cdeg) {
  const int64_t dt = (int64_t)d2 - ((int64_t)prom[5] << 8);
  const int64_t temp = 2000 + ((dt * prom[6]) / 8388608);
  int64_t off = ((int64_t)prom[2] << 17) + ((prom[4] * dt) / 64);
  int64_t sens = ((int64_t)prom[1] << 16) + ((prom[3] * dt) / 128);
  int64_t ti = 0;

  // Below 20 degrees C
  if (temp < 2000) {
    const int64_t t2 = (temp - 2000) * (temp - 2000);

    ti = (11 * dt * dt) / 34359738368LL;
    off -= (31 * t2) / 8;
    sens -= (63 * t2) / 32;
  }

  *pa = (int32_t)(((((int64_t)d1 * sens) / 2097152) - off) / 32768);
  *temp_cdeg = (int32_t)(temp - ti);
}
