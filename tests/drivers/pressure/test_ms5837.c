/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "ms5837_regs.h"

#include <string.h>

#include <clar.h>

// The PROM of the MS5837-02BA01 of a UNA Watch, as read on it: C0 0x7001, CRC 7, version 0
static const uint16_t s_watch_prom[MS5837_PROM_WORDS] = {0x7001, 0xa550, 0x9ce9, 0x6324,
                                                         0x63c0, 0x7d0c, 0x672b};

void test_ms5837__initialize(void) {
}

void test_ms5837__commands(void) {
  cl_assert_equal_i(MS5837_CMD_PROM(0), 0xA0);
  cl_assert_equal_i(MS5837_CMD_PROM(6), 0xAC);
  cl_assert_equal_i(MS5837_CMD_D1(0), 0x40);
  cl_assert_equal_i(MS5837_CMD_D1(4), 0x48);
  cl_assert_equal_i(MS5837_CMD_D2(4), 0x58);
  cl_assert_equal_i(MS5837_CMD_D2(5), 0x5A);
  cl_assert_equal_i(ms5837_conversion_us(4), 9040);
  cl_assert_equal_i(ms5837_conversion_us(9), 18080);
}

void test_ms5837__crc_of_a_watch(void) {
  cl_assert_equal_i(ms5837_crc4(s_watch_prom), 7);
  cl_assert_equal_i(ms5837_version(s_watch_prom), MS5837_VERSION_02BA01);
  cl_assert(ms5837_prom_valid(s_watch_prom));
}

void test_ms5837__prom_rejected(void) {
  uint16_t prom[MS5837_PROM_WORDS];

  // A bit off anywhere breaks the CRC
  for (unsigned int i = 0U; i < MS5837_PROM_WORDS; i++) {
    memcpy(prom, s_watch_prom, sizeof(prom));
    prom[i] ^= (i == 0U) ? 0x0010U : 0x0100U;
    cl_assert(!ms5837_prom_valid(prom));
  }

  // All zero passes the CRC, but is no part's
  memset(prom, 0, sizeof(prom));
  cl_assert_equal_i(ms5837_crc4(prom), 0);
  cl_assert(!ms5837_prom_valid(prom));

  // A 30BA, version 0x1A, with its CRC right
  memcpy(prom, s_watch_prom, sizeof(prom));
  prom[0] = (uint16_t)(0x1AU << 5);
  prom[0] |= (uint16_t)(ms5837_crc4(prom) << 12);
  cl_assert(!ms5837_prom_valid(prom));
}

// Against the data sheet's formulas worked out apart (integer division as C does it)
void test_ms5837__compensate(void) {
  int32_t pa, cdeg;

  // 23.30 degrees C, first order only
  ms5837_compensate(s_watch_prom, 6474800U, 8300000U, &pa, &cdeg);
  cl_assert_equal_i(pa, 101326);
  cl_assert_equal_i(cdeg, 2330);

  // Below 20 degrees C, the second order terms
  ms5837_compensate(s_watch_prom, 6474800U, 7900000U, &pa, &cdeg);
  cl_assert_equal_i(pa, 98665);
  cl_assert_equal_i(cdeg, 1044);

  ms5837_compensate(s_watch_prom, 6474800U, 7000000U, &pa, &cdeg);
  cl_assert_equal_i(pa, 91917);
  cl_assert_equal_i(cdeg, -2219);
}
