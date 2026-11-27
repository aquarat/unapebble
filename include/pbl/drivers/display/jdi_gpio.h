/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/display.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/panel.h>
#include <pbl/kernel/sem.h>
#include <pbl/kernel/thread.h>

/**
 * @defgroup drivers_display_jdi_gpio JDI parallel interface on GPIO
 * @ingroup drivers_display
 * @brief @ref drivers_display driver clocking a memory-in-pixel panel's JDI parallel interface
 * on GPIO.
 *
 * For a SoC without a controller generating the interface: the CPU clocks every frame out on
 * STM32 GPIO, the data pins on one port, as the UNA Watch firmware does. A frame takes about 60
 * ms of busy-waiting, which a thread of the driver does in the background, as a controller's DMA
 * would, at @c CONFIG_DISPLAY_JDI_GPIO_THREAD_PRIO: below the updating thread, so that a stream of
 * frames does not take the CPU from the threads of lower priority. An update is always the whole
 * frame, reported to the callback from that thread. The panel, from its port, powers up and
 * drives VCOM.
 *
 * The framebuffer may be larger than the image of the panel, which then shows its centre: the
 * round mask hides the rest, with the panel's own mask. The panel is mounted turned by its
 * rotation, counter-clockwise, which the driver undoes line by line.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
struct jdi_gpio_lut;

struct pbl_display_jdi_gpio_pin {
  volatile uint32_t *bsrr;
  uint32_t on;
  uint32_t off;
};

struct pbl_display_jdi_gpio_state {
  struct jdi_gpio_lut *lut;
  volatile uint32_t *data_bsrr;
  struct pbl_display_jdi_gpio_pin vst;
  struct pbl_display_jdi_gpio_pin vck;
  struct pbl_display_jdi_gpio_pin enb;
  struct pbl_display_jdi_gpio_pin xrst;
  struct pbl_display_jdi_gpio_pin hst;
  struct pbl_display_jdi_gpio_pin hck;
  uint32_t *msb;
  uint32_t *lsb;
  uint16_t rotation;
  /** The frame the thread sends next, given when start is. */
  const uint8_t *fb;
  uint32_t pitch;
  struct pbl_sem start;
  struct pbl_thread thread;
};
/** @endcond */

/** @brief Display clocking a JDI parallel interface on GPIO. */
struct pbl_display_jdi_gpio {
  /** Display. */
  struct pbl_display disp;
  /** Driver runtime state. */
  struct pbl_display_jdi_gpio_state *state;
  /** Panel. */
  const struct pbl_panel *panel;
  /** Round mask of the panel, or NULL. */
  const uint8_t *panel_mask;
  /** Round mask of the framebuffer, filled at init. */
  uint8_t *fb_mask;
  /** Pixels per panel line. */
  uint16_t hactive;
  /** Panel lines. */
  uint16_t vactive;
  /** Mounting rotation, in degrees counter-clockwise. */
  uint16_t rotation;
  /** VST (GSP), VCK (GCK), ENB (GEN), XRST (INTB), HST (BSP) and HCK (BCK). */
  struct pbl_gpio vst, vck, enb, xrst, hst, hck;
  /** R1, R2, G1, G2, B1 and B2, on one port. */
  struct pbl_gpio data[6];
  /** Stack of the thread sending the frames. */
  uint8_t *stack;
  size_t stack_size;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_display_ops pbl_display_jdi_gpio_ops;

#define PBL_DISPLAY_JDI_GPIO_SIDE(_size, _hactive, _vactive, _rotation, _turned) \
  (((_size) != 0) ? (_size)                                                      \
   : ((((_rotation) == 90) || ((_rotation) == 270)) == (_turned)) ? (_hactive)   \
                                                                  : (_vactive))
/** @endcond */

/**
 * @brief Define a display clocking a JDI parallel interface on GPIO.
 *
 * @param sym Symbol of the display.
 * @param _name Name.
 * @param _panel Panel.
 * @param _hactive Pixels per panel line.
 * @param _vactive Panel lines.
 * @param _rotation Mounting rotation of the panel: 0, 90, 180 or 270 degrees counter-clockwise.
 * @param _round_mask Round mask of the panel, or NULL.
 * @param _width Framebuffer width, 0 for that of the image.
 * @param _height Framebuffer height, 0 for that of the image.
 * @param _vst VST pin, from PBL_GPIO().
 * @param _vck VCK pin.
 * @param _enb ENB pin.
 * @param _xrst XRST pin.
 * @param _hst HST pin.
 * @param _hck HCK pin.
 * @param ... R1, R2, G1, G2, B1 and B2 pins.
 */
#define PBL_DISPLAY_JDI_GPIO_DEFINE(sym, _name, _panel, _hactive, _vactive, _rotation,              \
                                    _round_mask, _width, _height, _vst, _vck, _enb, _xrst, _hst,    \
                                    _hck, ...)                                                      \
  static_assert((_rotation) == 0 || (_rotation) == 90 || (_rotation) == 180 || (_rotation) == 270, \
                "rotation of 0, 90, 180 or 270 degrees");                                           \
  static_assert(((_hactive) % 2) == 0, "pixels go in pairs");                                       \
  PBL_DISPLAY_STATE_DEFINE(sym);                                                                    \
  PBL_THREAD_STACK_DEFINE(sym##_stack, CONFIG_DISPLAY_JDI_GPIO_THREAD_STACK_SIZE);                  \
  static uint32_t sym##_planes[2][(_hactive) / 2];                                                  \
  static struct pbl_display_jdi_gpio_state sym##_jdi_state = {                                      \
    .msb = sym##_planes[0],                                                                         \
    .lsb = sym##_planes[1],                                                                         \
  };                                                                                                \
  static uint8_t                                                                                    \
      sym##_fb_mask[PBL_DISPLAY_JDI_GPIO_SIDE(_height, _hactive, _vactive, _rotation, true) / 2];   \
  PBL_DEVICE_INSTANCE(sym, struct pbl_display_jdi_gpio, struct pbl_display, disp){                  \
    .disp = PBL_DISPLAY_INIT(                                                                       \
        sym, _name, nullptr, &pbl_display_jdi_gpio_ops,                                             \
        PBL_DISPLAY_JDI_GPIO_SIDE(_width, _hactive, _vactive, _rotation, false),                    \
        PBL_DISPLAY_JDI_GPIO_SIDE(_height, _hactive, _vactive, _rotation, true),                    \
        PBL_DISPLAY_FORMAT_ARGB2222, false, .round_mask = sym##_fb_mask),                           \
    .state = &sym##_jdi_state, .panel = (_panel), .panel_mask = (_round_mask),                      \
    .fb_mask = sym##_fb_mask, .hactive = (_hactive), .vactive = (_vactive),                         \
    .rotation = (_rotation), .vst = _vst, .vck = _vck, .enb = _enb, .xrst = _xrst, .hst = _hst,     \
    .hck = _hck, .data = {__VA_ARGS__}, .stack = sym##_stack, .stack_size = sizeof(sym##_stack),    \
  }

/** @} */
