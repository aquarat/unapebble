/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "jdi_gpio_line.h"

#include <string.h>

// Bit of each color in an ARGB2222 pixel: the most significant one, the least one under it
#define RED_MSB   5U
#define GREEN_MSB 3U
#define BLUE_MSB  1U

static uint16_t prv_pins(const uint8_t pins[JDI_GPIO_NUM_DATA], unsigned int pixel, uint8_t color,
                         unsigned int plane) {
  const unsigned int r = (color >> (RED_MSB - plane)) & 1U;
  const unsigned int g = (color >> (GREEN_MSB - plane)) & 1U;
  const unsigned int b = (color >> (BLUE_MSB - plane)) & 1U;

  return (uint16_t)((r << pins[JDI_GPIO_R1 + pixel]) | (g << pins[JDI_GPIO_G1 + pixel]) |
                    (b << pins[JDI_GPIO_B1 + pixel]));
}

void jdi_gpio_lut_init(struct jdi_gpio_lut *lut, const uint8_t pins[JDI_GPIO_NUM_DATA]) {
  memset(lut, 0, sizeof(*lut));

  for (unsigned int i = 0U; i < JDI_GPIO_NUM_DATA; i++) {
    lut->mask |= (uint16_t)(1U << pins[i]);
  }

  for (unsigned int c = 0U; c < 256U; c++) {
    for (unsigned int pixel = 0U; pixel < 2U; pixel++) {
      lut->msb[pixel][c] = prv_pins(pins, pixel, (uint8_t)c, 0U);
      lut->lsb[pixel][c] = prv_pins(pins, pixel, (uint8_t)c, 1U);
    }
  }
}

struct jdi_gpio_src jdi_gpio_line_src(uint16_t rotation, uint16_t hactive, uint16_t vactive,
                                      uint32_t pitch, uint16_t ox, uint16_t oy, uint16_t line) {
  const int32_t p = (int32_t)pitch;
  const int32_t x0 = ox;
  const int32_t y0 = oy;
  const int32_t l = line;

  switch (rotation) {
    case 90:
      // The image is vactive wide and hactive high
      return (struct jdi_gpio_src){(y0 * p) + x0 + (vactive - 1 - l), p};
    case 180:
      return (struct jdi_gpio_src){((y0 + vactive - 1 - l) * p) + x0 + (hactive - 1), -1};
    case 270:
      return (struct jdi_gpio_src){((y0 + hactive - 1) * p) + x0 + l, -p};
    default:
      return (struct jdi_gpio_src){((y0 + l) * p) + x0, 1};
  }
}

void jdi_gpio_line(const struct jdi_gpio_lut *lut, const uint8_t *src, int32_t step, uint16_t n,
                   uint32_t *msb, uint32_t *lsb) {
  const uint32_t mask = lut->mask;
  int32_t offs = 0;

  for (uint16_t k = 0U; k < (n / 2U); k++) {
    const uint8_t p0 = src[offs];
    const uint8_t p1 = src[offs + step];
    const uint32_t m = (uint32_t)lut->msb[0][p0] | lut->msb[1][p1];
    const uint32_t l = (uint32_t)lut->lsb[0][p0] | lut->lsb[1][p1];

    msb[k] = m | ((m ^ mask) << 16);
    lsb[k] = l | ((l ^ mask) << 16);
    offs += 2 * step;
  }
}
