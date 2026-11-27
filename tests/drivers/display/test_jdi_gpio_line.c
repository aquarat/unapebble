/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <stdlib.h>
#include <string.h>

#include "jdi_gpio_line.h"

// The UNA Watch: a 240 x 240 panel turned a quarter, the 260 x 260 gabbro framebuffer around it,
// the data pins on PE0, PE8, PE2, PE10, PE4 and PE12 (R1, R2, G1, G2, B1, B2)
#define FB      260U
#define PANEL   240U
#define MARGIN  ((FB - PANEL) / 2U)
#define WORDS   (PANEL / 2U)
#define PE_MASK 0x1515U

static const uint8_t s_pins[JDI_GPIO_NUM_DATA] = {0, 8, 2, 10, 4, 12};

static struct jdi_gpio_lut s_lut;
static uint8_t s_fb[FB * FB];

/*
 * The stock firmware's path, transcribed from una-kernel-public
 * Software/Libs/Source/Hardware/LS012B7DD06A.cpp: a 240 x 240 ABGR2222 frame (red in bits 1:0,
 * blue in 5:4) turned by RotateQuarterTurn() (:42-49), each line then read as little-endian
 * halfwords, written whole to GPIOE->ODR, shifted right once for the MSB plane (:290-337).
 */
static uint8_t s_una_frame[PANEL * PANEL];
static uint8_t s_una_rotated[PANEL * PANEL];

static void prv_una_rotate(const uint8_t *src, uint8_t *dst) {
  for (int32_t src_y = 0; src_y < (int32_t)PANEL; src_y++) {
    for (int32_t src_x = 0; src_x < (int32_t)PANEL; src_x++) {
      dst[(PANEL - 1 - src_x) * PANEL + src_y] = src[src_y * PANEL + src_x];
    }
  }
}

static uint16_t prv_una_odr(const uint8_t *line, uint16_t k, bool msb) {
  const uint16_t halfword = (uint16_t)(line[2U * k] | (line[(2U * k) + 1U] << 8));

  return (uint16_t)(halfword >> (msb ? 1U : 0U));
}

// ARGB2222, red in bits 5:4, to the ABGR2222 of the stock firmware
static uint8_t prv_to_abgr(uint8_t argb) {
  return (uint8_t)((argb & 0xCCU) | ((argb >> 4) & 0x03U) | ((argb & 0x03U) << 4));
}

static void prv_check_line(uint16_t rotation, uint16_t line, const uint8_t *una_line) {
  const struct jdi_gpio_src src = jdi_gpio_line_src(rotation, PANEL, PANEL, FB, MARGIN, MARGIN,
                                                    line);
  uint32_t msb[WORDS];
  uint32_t lsb[WORDS];

  jdi_gpio_line(&s_lut, &s_fb[src.start], src.step, PANEL, msb, lsb);

  for (uint16_t k = 0U; k < WORDS; k++) {
    const uint16_t una_msb = prv_una_odr(una_line, k, true) & PE_MASK;
    const uint16_t una_lsb = prv_una_odr(una_line, k, false) & PE_MASK;

    // The data pins the stock firmware sets, and the others of them cleared
    cl_assert_equal_i(msb[k], una_msb | ((uint32_t)(una_msb ^ PE_MASK) << 16));
    cl_assert_equal_i(lsb[k], una_lsb | ((uint32_t)(una_lsb ^ PE_MASK) << 16));
  }
}

void test_jdi_gpio_line__initialize(void) {
  srand(42);
  for (size_t i = 0U; i < sizeof(s_fb); i++) {
    s_fb[i] = (uint8_t)rand();
  }

  for (uint32_t y = 0U; y < PANEL; y++) {
    for (uint32_t x = 0U; x < PANEL; x++) {
      s_una_frame[y * PANEL + x] = prv_to_abgr(s_fb[(y + MARGIN) * FB + x + MARGIN]);
    }
  }
  prv_una_rotate(s_una_frame, s_una_rotated);

  jdi_gpio_lut_init(&s_lut, s_pins);
}

void test_jdi_gpio_line__pin_mask(void) {
  cl_assert_equal_i(s_lut.mask, PE_MASK);
}

void test_jdi_gpio_line__colors_on_their_pins(void) {
  // Red in an ARGB2222 pixel goes out on R1 for the even pixel and R2 for the odd one
  cl_assert_equal_i(s_lut.msb[0][0x20], 1U << 0);
  cl_assert_equal_i(s_lut.lsb[0][0x10], 1U << 0);
  cl_assert_equal_i(s_lut.msb[1][0x20], 1U << 8);
  cl_assert_equal_i(s_lut.msb[0][0x08], 1U << 2);
  cl_assert_equal_i(s_lut.lsb[1][0x04], 1U << 10);
  cl_assert_equal_i(s_lut.msb[0][0x02], 1U << 4);
  cl_assert_equal_i(s_lut.lsb[1][0x01], 1U << 12);
  // Alpha goes nowhere
  cl_assert_equal_i(s_lut.msb[0][0xC0], 0U);
  cl_assert_equal_i(s_lut.lsb[1][0xC0], 0U);
}

void test_jdi_gpio_line__matches_the_stock_firmware(void) {
  for (uint16_t line = 0U; line < PANEL; line++) {
    prv_check_line(90U, line, &s_una_rotated[line * PANEL]);
  }
}

void test_jdi_gpio_line__other_rotations(void) {
  uint8_t ref[PANEL];

  for (uint16_t line = 0U; line < PANEL; line += 7U) {
    // Unturned: the rows of the image
    prv_check_line(0U, line, &s_una_frame[line * PANEL]);

    // Turned by half: the rows from the bottom, right to left
    for (uint16_t p = 0U; p < PANEL; p++) {
      ref[p] = s_una_frame[(PANEL - 1U - line) * PANEL + (PANEL - 1U - p)];
    }
    prv_check_line(180U, line, ref);

    // Three quarters: a quarter, then a half, so the lines of a quarter turn backwards
    for (uint16_t p = 0U; p < PANEL; p++) {
      ref[p] = s_una_rotated[(PANEL - 1U - line) * PANEL + (PANEL - 1U - p)];
    }
    prv_check_line(270U, line, ref);
  }
}

void test_jdi_gpio_line__clear_repeats_one_pixel(void) {
  const uint8_t black = 0U;
  uint32_t msb[WORDS];
  uint32_t lsb[WORDS];

  jdi_gpio_line(&s_lut, &black, 0, PANEL, msb, lsb);
  for (uint16_t k = 0U; k < WORDS; k++) {
    cl_assert_equal_i(msb[k], (uint32_t)PE_MASK << 16);
    cl_assert_equal_i(lsb[k], (uint32_t)PE_MASK << 16);
  }
}
