/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "jdi_gpio_line.h"

#include <errno.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/display/jdi_gpio.h>
#include <pbl/drivers/gpio/stm32.h>
#include <pbl/kernel/compiler.h>
#include <pbl/kernel/time.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

#include <hardware/stm32_gpio.h>

/*
 * Timing of the UNA Watch's LS012B7DD06A, from the stock driver
 * (una-kernel-public Software/Libs/Source/Hardware/LS012B7DD06A.cpp:290-396), whose comments
 * give the datasheet minimums. The stock driver relies on unoptimized code for the data setup
 * and hold times; here every one is waited for on the cycle counter.
 */
// INTB high to GSP high, thsINTB; GSP high to the first GCK edge, thsGSP (:358-361)
#define T_HS_INTB_NS 11000U
#define T_HS_GSP_NS  11000U
// GCK half period, thwGCK and tlwGCK, at least 68.89 us (:363, :292)
#define T_GCK_NS 69000U
// GCK low to GSP low, tlsGSP (:369-370)
#define T_LS_GSP_NS 11000U
// GCK edge to BSP high, thsGCK (:296)
#define T_HS_GCK_NS 440U
// BSP high to the first BCK edge, thsBSP; BSP low after BCK, tlsBSP (:299, :306)
#define T_BSP_NS 220U
// The first BCK pulse: shorter loses the first pixels of the line (:301)
#define T_BCK_START_NS 1000U
// Data setup and hold around each BCK edge, tsRGB and thRGB, 220 ns each (:313-319), so that
// the BCK pulse is at least its 440 ns
#define T_S_RGB_NS 250U
#define T_H_RGB_NS 250U
// BCK pulse width, thwBCK (:330)
#define T_W_BCK_NS 440U
// GEN pulse, twhGEN of 10.64 us, and GEN low to the next GCK edge, thhGCK of 4.4 us (:334-336)
#define T_W_GEN_NS  11000U
#define T_HH_GCK_NS 5000U

// The ARGB2222 tables of the data pins, which every instance shares the port layout of
static struct jdi_gpio_lut s_lut;

struct timing {
  uint32_t hs_intb;
  uint32_t hs_gsp;
  uint32_t gck;
  uint32_t ls_gsp;
  uint32_t hs_gck;
  uint32_t bsp;
  uint32_t bck_start;
  uint32_t s_rgb;
  uint32_t h_rgb;
  uint32_t w_bck;
  uint32_t w_gen;
  uint32_t hh_gck;
};

static const struct pbl_display_jdi_gpio *prv_jdi(const struct pbl_display *disp) {
  return PBL_CONTAINER_OF(disp, const struct pbl_display_jdi_gpio, disp);
}

static PBL_ALWAYS_INLINE void prv_spin(uint32_t since, uint32_t cycles) {
  while ((pbl_cycle_get_32() - since) < cycles) {
  }
}

// Waits after a write, so that the next one comes at least that long after it
static PBL_ALWAYS_INLINE void prv_wait(uint32_t cycles) {
  prv_spin(pbl_cycle_get_32(), cycles);
}

static PBL_ALWAYS_INLINE void prv_on(const struct pbl_display_jdi_gpio_pin *pin) {
  *pin->bsrr = pin->on;
}

static PBL_ALWAYS_INLINE void prv_off(const struct pbl_display_jdi_gpio_pin *pin) {
  *pin->bsrr = pin->off;
}

static void prv_timing(struct timing *t) {
  const uint32_t mhz = pbl_cycles_per_sec() / 1000000U;

#define CYCLES(ns) ((((ns) * mhz) + 999U) / 1000U)
  *t = (struct timing){
    .hs_intb = CYCLES(T_HS_INTB_NS),
    .hs_gsp = CYCLES(T_HS_GSP_NS),
    .gck = CYCLES(T_GCK_NS),
    .ls_gsp = CYCLES(T_LS_GSP_NS),
    .hs_gck = CYCLES(T_HS_GCK_NS),
    .bsp = CYCLES(T_BSP_NS),
    .bck_start = CYCLES(T_BCK_START_NS),
    .s_rgb = CYCLES(T_S_RGB_NS),
    .h_rgb = CYCLES(T_H_RGB_NS),
    .w_bck = CYCLES(T_W_BCK_NS),
    .w_gen = CYCLES(T_W_GEN_NS),
    .hh_gck = CYCLES(T_HH_GCK_NS),
  };
#undef CYCLES
}

// One bit plane of a line, during a half period of GCK: two pixels per BCK edge, then the GEN
// pulse that writes them (LS012B7DD06A.cpp:290-337)
static void prv_plane(struct pbl_display_jdi_gpio_state *st, const struct timing *t,
                      const uint32_t *plane, uint16_t words) {
  volatile uint32_t *const data = st->data_bsrr;

  prv_wait(t->hs_gck);
  prv_on(&st->hst);
  prv_wait(t->bsp);
  prv_on(&st->hck);
  prv_wait(t->bck_start);
  prv_off(&st->hck);
  prv_wait(t->bsp);
  prv_off(&st->hst);
  prv_wait(t->bsp);

  for (uint16_t k = 0U; k < words; k += 2U) {
    *data = plane[k];
    prv_wait(t->s_rgb);
    prv_on(&st->hck);
    prv_wait(t->h_rgb);
    *data = plane[k + 1U];
    prv_wait(t->s_rgb);
    prv_off(&st->hck);
    prv_wait(t->h_rgb);
  }

  prv_on(&st->hck);
  prv_on(&st->enb);
  prv_wait(t->w_bck);
  prv_off(&st->hck);
  prv_wait(t->w_gen);
  prv_off(&st->enb);
  prv_wait(t->hh_gck);
}

static void prv_convert(const struct pbl_display_jdi_gpio *jdi, const uint8_t *fb, uint32_t pitch,
                        uint16_t line) {
  const struct pbl_display_props *props = &jdi->disp.props;
  struct pbl_display_jdi_gpio_state *st = jdi->state;
  const bool turned = (st->rotation == 90U) || (st->rotation == 270U);
  const uint16_t w = turned ? jdi->vactive : jdi->hactive;
  const uint16_t h = turned ? jdi->hactive : jdi->vactive;
  struct jdi_gpio_src src = {0, 0};

  // A clear sends the first byte of a row of zeros, again and again
  if (pitch != 0U) {
    src = jdi_gpio_line_src(st->rotation, jdi->hactive, jdi->vactive, pitch,
                            (props->width - w) / 2U, (props->height - h) / 2U, line);
  }

  jdi_gpio_line(st->lut, &fb[src.start], src.step, jdi->hactive, st->msb, st->lsb);
}

// The whole frame, every line once with its MSB plane while GCK is low and its LSB plane while
// it is high, the next line converted meanwhile (LS012B7DD06A.cpp:339-396)
static void prv_frame(const struct pbl_display_jdi_gpio *jdi, const uint8_t *fb, uint32_t pitch) {
  struct pbl_display_jdi_gpio_state *st = jdi->state;
  const uint16_t words = jdi->hactive / 2U;
  struct timing t;
  uint32_t edge;

  prv_timing(&t);
  prv_convert(jdi, fb, pitch, 0U);

  prv_on(&st->xrst);
  prv_wait(t.hs_intb);
  prv_on(&st->vst);
  prv_wait(t.hs_gsp);
  prv_on(&st->vck);
  prv_wait(t.gck);

  for (uint16_t line = 0U; line < jdi->vactive; line++) {
    prv_off(&st->vck);
    edge = pbl_cycle_get_32();
    prv_plane(st, &t, st->msb, words);
    if (line == 0U) {
      prv_off(&st->vst);
      prv_wait(t.ls_gsp);
    }
    prv_spin(edge, t.gck);

    prv_on(&st->vck);
    edge = pbl_cycle_get_32();
    prv_plane(st, &t, st->lsb, words);
    if ((line + 1U) < jdi->vactive) {
      prv_convert(jdi, fb, pitch, line + 1U);
    }
    prv_spin(edge, t.gck);
  }

  prv_off(&st->vck);
  prv_wait(t.gck);
  prv_on(&st->vck);
  prv_wait(t.gck);
  prv_off(&st->vck);
  prv_off(&st->xrst);
}

static int prv_pin_init(const struct pbl_gpio *gpio, struct pbl_display_jdi_gpio_pin *pin) {
  const struct pbl_gpio_stm32 *port = PBL_CONTAINER_OF(gpio->port, const struct pbl_gpio_stm32, port);
  const bool active_low = (gpio->flags & PBL_GPIO_ACTIVE_LOW) != 0U;
  int ret;

  ret = pbl_device_init(&gpio->port->dev);
  if (ret == 0) {
    ret = pbl_gpio_configure(gpio, PBL_GPIO_OUTPUT_INACTIVE);
  }
  if (ret != 0) {
    return ret;
  }

  pin->bsrr = (volatile uint32_t *)(port->base + STM32_GPIO_BSRR_OFFSET);
  pin->on = active_low ? GPIO_BSRR_RESET(gpio->pin) : GPIO_BSRR_SET(gpio->pin);
  pin->off = active_low ? GPIO_BSRR_SET(gpio->pin) : GPIO_BSRR_RESET(gpio->pin);

  return 0;
}

// What the panel does not show: the rows and columns around a smaller image, and what the
// panel's own mask hides
static void prv_fb_mask(const struct pbl_display_jdi_gpio *jdi) {
  const struct pbl_display_props *props = &jdi->disp.props;
  const bool turned = (jdi->rotation == 90U) || (jdi->rotation == 270U);
  const uint16_t w = turned ? jdi->vactive : jdi->hactive;
  const uint16_t h = turned ? jdi->hactive : jdi->vactive;
  const uint16_t ox = (props->width - w) / 2U;
  const uint16_t oy = (props->height - h) / 2U;

  for (uint16_t y = 0U; y < props->height / 2U; y++) {
    uint16_t inset = props->width / 2U;

    if (y >= oy) {
      inset = ox + ((jdi->panel_mask != nullptr) ? jdi->panel_mask[y - oy] : 0U);
    }

    jdi->fb_mask[y] = (uint8_t)PBL_MIN(inset, props->width / 2U);
  }
}

static int prv_init(const struct pbl_display *disp) {
  const struct pbl_display_jdi_gpio *jdi = prv_jdi(disp);
  struct pbl_display_jdi_gpio_state *st = jdi->state;
  const bool turned = (jdi->rotation == 90U) || (jdi->rotation == 270U);
  struct pbl_display_jdi_gpio_pin data[JDI_GPIO_NUM_DATA];
  uint8_t pins[JDI_GPIO_NUM_DATA];
  int ret;

  if (disp->props.width < (turned ? jdi->vactive : jdi->hactive) ||
      disp->props.height < (turned ? jdi->hactive : jdi->vactive)) {
    return -EINVAL;
  }

  ret = prv_pin_init(&jdi->vst, &st->vst);
  if (ret == 0) {
    ret = prv_pin_init(&jdi->vck, &st->vck);
  }
  if (ret == 0) {
    ret = prv_pin_init(&jdi->enb, &st->enb);
  }
  if (ret == 0) {
    ret = prv_pin_init(&jdi->xrst, &st->xrst);
  }
  if (ret == 0) {
    ret = prv_pin_init(&jdi->hst, &st->hst);
  }
  if (ret == 0) {
    ret = prv_pin_init(&jdi->hck, &st->hck);
  }

  // One write sets every data pin: they share a port, active high
  for (size_t i = 0U; ret == 0 && i < PBL_ARRAY_SIZE(jdi->data); i++) {
    ret = prv_pin_init(&jdi->data[i], &data[i]);
    if (ret == 0 && (data[i].bsrr != data[0].bsrr ||
                     (jdi->data[i].flags & PBL_GPIO_ACTIVE_LOW) != 0U)) {
      ret = -EINVAL;
    }
    pins[i] = jdi->data[i].pin;
  }
  if (ret != 0) {
    return ret;
  }

  jdi_gpio_lut_init(&s_lut, pins);
  st->lut = &s_lut;
  st->data_bsrr = data[0].bsrr;
  st->rotation = jdi->rotation;
  prv_fb_mask(jdi);

  ret = pbl_device_init(&jdi->panel->dev);
  if (ret == 0) {
    ret = pbl_panel_prepare(jdi->panel);
  }
  if (ret != 0) {
    return ret;
  }

  // The panel keeps what it showed powered off, or noise: start from black
  prv_frame(jdi, (const uint8_t[]){0U}, 0U);

  return pbl_panel_enable(jdi->panel);
}

static int prv_enable(const struct pbl_display *disp) {
  return pbl_panel_enable(prv_jdi(disp)->panel);
}

static int prv_disable(const struct pbl_display *disp) {
  return pbl_panel_disable(prv_jdi(disp)->panel);
}

static int prv_set_rotated(const struct pbl_display *disp, bool rotated) {
  const struct pbl_display_jdi_gpio *jdi = prv_jdi(disp);

  jdi->state->rotation = (jdi->rotation + (rotated ? 180U : 0U)) % 360U;

  return 0;
}

static int prv_update(const struct pbl_display *disp, const struct pbl_display_buffer *buf,
                      uint16_t y0, uint16_t y1) {
  // Each panel line takes a column of a framebuffer turned a quarter: send them all
  prv_frame(prv_jdi(disp), buf->data, buf->pitch);
  pbl_display_complete(disp, 0);

  return 0;
}

static int prv_clear(const struct pbl_display *disp) {
  prv_frame(prv_jdi(disp), (const uint8_t[]){0U}, 0U);

  return 0;
}

const struct pbl_display_ops pbl_display_jdi_gpio_ops = {
  .init = prv_init,
  .enable = prv_enable,
  .disable = prv_disable,
  .set_rotated = prv_set_rotated,
  .update = prv_update,
  .clear = prv_clear,
};

#include PBL_DT_INSTANCES(jdi_parallel_gpio)
