/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

/*
 * Framebuffer to JDI parallel interface conversion, for drivers clocking the interface on GPIO.
 *
 * A panel line carries two pixels per HCK edge, 2k on R1/G1/B1 and 2k+1 on R2/G2/B2, first
 * the most significant bit of each color for the whole line while VCK is low, then the least
 * significant one while VCK is high. Pixels come from an ARGB2222 framebuffer: red in bits 5:4,
 * green in 3:2, blue in 1:0, alpha ignored.
 *
 * The data pins sit on one GPIO port: each word of a plane is the value of a set/reset
 * register, the pins of the colors set in its low half and the others in its high half, so one
 * write gives the pins of a pair of pixels whatever else the port drives.
 */

enum jdi_gpio_pin {
  JDI_GPIO_R1,
  JDI_GPIO_R2,
  JDI_GPIO_G1,
  JDI_GPIO_G2,
  JDI_GPIO_B1,
  JDI_GPIO_B2,
  JDI_GPIO_NUM_DATA,
};

/** Pins of each color bit, for the first and second pixel of a pair. */
struct jdi_gpio_lut {
  uint16_t msb[2][256];
  uint16_t lsb[2][256];
  // Every data pin
  uint16_t mask;
};

/** Where a panel line comes from in the framebuffer. */
struct jdi_gpio_src {
  int32_t start;
  int32_t step;
};

/** Build the tables for data pins @p pins, numbers on their port, in enum jdi_gpio_pin order. */
void jdi_gpio_lut_init(struct jdi_gpio_lut *lut, const uint8_t pins[JDI_GPIO_NUM_DATA]);

/**
 * Locate line @p line of a panel of @p hactive pixels by @p vactive lines, mounted turned by
 * @p rotation degrees counter-clockwise, in a framebuffer of @p pitch bytes per row, the image
 * at @p ox, @p oy: the offset of its first pixel and the step from one pixel to the next.
 *
 * At 90 degrees, line L pixel P shows the image pixel x = vactive - 1 - L, y = P: the right
 * column of the image is line 0, top to bottom, as the UNA Watch mounts its panel.
 */
struct jdi_gpio_src jdi_gpio_line_src(uint16_t rotation, uint16_t hactive, uint16_t vactive,
                                      uint32_t pitch, uint16_t ox, uint16_t oy, uint16_t line);

/**
 * Convert a line of @p n pixels, @p n even, from @p src, @p step bytes apart, into its two
 * planes of @p n / 2 set/reset words.
 */
void jdi_gpio_line(const struct jdi_gpio_lut *lut, const uint8_t *src, int32_t step, uint16_t n,
                   uint32_t *msb, uint32_t *lsb);
