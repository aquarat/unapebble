/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "max17262_regs.h"

#include <string.h>

#include <clar.h>

// The UNA Watch's cell, as its devicetree describes it
#define CAPACITY_UAH 200000U
#define TERM_UA      14000U
#define EMPTY_UV     3400000U
#define RECOVERY_UV  3960000U
#define CHARGE_UV    4360000U

static uint16_t s_model[MAX17262_MODEL_COUNT];
static uint8_t s_block[2U * MAX17262_READ_COUNT];

static void prv_set(uint8_t reg, uint16_t val) {
  s_block[2U * reg] = (uint8_t)val;
  s_block[(2U * reg) + 1U] = (uint8_t)(val >> 8);
}

void test_max17262__initialize(void) {
  max17262_model_encode(CAPACITY_UAH, TERM_UA, EMPTY_UV, RECOVERY_UV, CHARGE_UV, 80U, s_model);

  // A gauge at 50%, 3.8 V, charging at 50 mA, 25 degrees C
  memset(s_block, 0, sizeof(s_block));
  prv_set(MAX17262_DESIGNCAP, s_model[MAX17262_MODEL_DESIGNCAP]);
  prv_set(MAX17262_REPSOC, 50U * 256U);
  prv_set(MAX17262_VCELL, 48640U);
  prv_set(MAX17262_AVGVCELL, 48640U);
  prv_set(MAX17262_CURRENT, 320U);
  prv_set(MAX17262_TEMP, 25U * 256U);
}

// The registers the stock firmware writes for the same cell (FuelGauge.cpp, MAX17262.hpp
// encoders, main.h:238-250), so neither firmware reloads the model the other left
void test_max17262__model_matches_stock_firmware(void) {
  cl_assert_equal_i(s_model[MAX17262_MODEL_DESIGNCAP], 0x0190);
  cl_assert_equal_i(s_model[MAX17262_MODEL_ICHGTERM], 0x0059);
  cl_assert_equal_i(s_model[MAX17262_MODEL_VEMPTY], 0xAA63);
  cl_assert_equal_i(s_model[MAX17262_MODEL_FULLSOCTHR], 0x5005);
  cl_assert_equal_i(s_model[MAX17262_MODEL_SOCHOLD], 0x1000);
  cl_assert_equal_i(s_model[MAX17262_MODEL_MODELCFG], 0x0400);
}

void test_max17262__model_registers(void) {
  cl_assert_equal_i(max17262_model_addr[MAX17262_MODEL_DESIGNCAP], 0x18);
  cl_assert_equal_i(max17262_model_addr[MAX17262_MODEL_ICHGTERM], 0x1E);
  cl_assert_equal_i(max17262_model_addr[MAX17262_MODEL_VEMPTY], 0x3A);
  cl_assert_equal_i(max17262_model_addr[MAX17262_MODEL_FULLSOCTHR], 0x13);
  cl_assert_equal_i(max17262_model_addr[MAX17262_MODEL_SOCHOLD], 0xD3);
  // Last: writing it starts the refresh
  cl_assert_equal_i(max17262_model_addr[MAX17262_MODEL_MODELCFG], 0xDB);
}

void test_max17262__model_vchg(void) {
  uint16_t model[MAX17262_MODEL_COUNT];

  max17262_model_encode(CAPACITY_UAH, TERM_UA, EMPTY_UV, RECOVERY_UV, 4200000U, 95U, model);
  cl_assert_equal_i(model[MAX17262_MODEL_MODELCFG], 0);
  cl_assert_equal_i(model[MAX17262_MODEL_FULLSOCTHR], 0x5F05);
  max17262_model_encode(CAPACITY_UAH, TERM_UA, EMPTY_UV, RECOVERY_UV, 4250000U, 80U, model);
  cl_assert_equal_i(model[MAX17262_MODEL_MODELCFG], 0);
  max17262_model_encode(CAPACITY_UAH, TERM_UA, EMPTY_UV, RECOVERY_UV, 4400000U, 80U, model);
  cl_assert_equal_i(model[MAX17262_MODEL_MODELCFG], 0x0400);
}

void test_max17262__load_only_when_lost(void) {
  uint16_t actual[MAX17262_MODEL_COUNT];

  memcpy(actual, s_model, sizeof(actual));
  // Kept across a reset of the watch: what it learned stays
  cl_assert(!max17262_model_needs_load(0x0000, actual, s_model));
  cl_assert(!max17262_model_needs_load(0x8080, actual, s_model));
  // Power-on reset: the model is gone
  cl_assert(max17262_model_needs_load(MAX17262_STATUS_POR, actual, s_model));

  for (unsigned int i = 0U; i < MAX17262_MODEL_COUNT; i++) {
    memcpy(actual, s_model, sizeof(actual));
    actual[i] ^= 1U;
    cl_assert(max17262_model_needs_load(0x0000, actual, s_model));
  }
}

void test_max17262__decode(void) {
  struct pbl_gauge_reading r;

  cl_assert_equal_i(max17262_decode(s_block, s_model[MAX17262_MODEL_DESIGNCAP], &r), 0);
  cl_assert_equal_i(r.soc_cpct, 5000);
  cl_assert_equal_i(r.voltage_mv, 3800);
  cl_assert_equal_i(r.avg_voltage_mv, 3800);
  // Charging reads negative
  cl_assert_equal_i(r.current_ua, -50000);
  cl_assert_equal_i(r.temp_mc, 25000);
}

void test_max17262__decode_signs_and_rounding(void) {
  struct pbl_gauge_reading r;

  // Discharging 10 mA, -5.5 degrees C, 99.996% (shown as less than 100)
  prv_set(MAX17262_CURRENT, (uint16_t)(int16_t)-64);
  prv_set(MAX17262_TEMP, (uint16_t)(int16_t)(-1408));
  prv_set(MAX17262_REPSOC, (100U * 256U) - 1U);
  cl_assert_equal_i(max17262_decode(s_block, s_model[MAX17262_MODEL_DESIGNCAP], &r), 0);
  cl_assert_equal_i(r.current_ua, 10000);
  cl_assert_equal_i(r.temp_mc, -5500);
  cl_assert_equal_i(r.soc_cpct, 9999);

  // Above 100% is 100%
  prv_set(MAX17262_REPSOC, 0xFFFF);
  cl_assert_equal_i(max17262_decode(s_block, s_model[MAX17262_MODEL_DESIGNCAP], &r), 0);
  cl_assert_equal_i(r.soc_cpct, 10000);
}

void test_max17262__decode_refuses(void) {
  struct pbl_gauge_reading r;
  const uint16_t cap = s_model[MAX17262_MODEL_DESIGNCAP];

  prv_set(MAX17262_STATUS, MAX17262_STATUS_POR);
  cl_assert_equal_i(max17262_decode(s_block, cap, &r), -EAGAIN);
  prv_set(MAX17262_STATUS, MAX17262_STATUS_BST);
  cl_assert_equal_i(max17262_decode(s_block, cap, &r), -ENODEV);
  prv_set(MAX17262_STATUS, 0);

  // Another model, or a bus that reads all ones or all zeros
  cl_assert_equal_i(max17262_decode(s_block, cap + 1U, &r), -EIO);
  memset(s_block, 0xFF, sizeof(s_block));
  cl_assert(max17262_decode(s_block, cap, &r) != 0);
  memset(s_block, 0x00, sizeof(s_block));
  cl_assert(max17262_decode(s_block, cap, &r) != 0);

  // Implausible voltage or temperature
  test_max17262__initialize();
  prv_set(MAX17262_VCELL, 0);
  cl_assert_equal_i(max17262_decode(s_block, cap, &r), -EIO);
  test_max17262__initialize();
  prv_set(MAX17262_TEMP, 90U * 256U);
  cl_assert_equal_i(max17262_decode(s_block, cap, &r), -EIO);
}
