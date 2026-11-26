/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The 240 x 240 memory-in-pixel panel, clocked by the CPU on GPIO as PebbleOS
 * does it (drivers/display/jdi_gpio.c, with the timings of the stock driver,
 * LS012B7DD06A.cpp:290-396), and its VCOM on LPTIM1 (FRP, PB2) and LPTIM3
 * (XFRP, PD14) at 30.7 Hz. The panel is mounted a quarter turn round: line l
 * is column 239 - l of the upright image, top to bottom. The framebuffer is
 * ARGB2222, as PebbleOS's.
 */

#include "font8x8.h"
#include "regs.h"
#include "stub.h"

#include <string.h>

uint8_t g_fb[240 * 240] __attribute__((section(".noinit"), aligned(4)));

#define W 240U
#define H 240U

#define T_HS_INTB_NS   11000U
#define T_HS_GSP_NS    11000U
#define T_GCK_NS       69000U
#define T_LS_GSP_NS    11000U
#define T_HS_GCK_NS    440U
#define T_BSP_NS       220U
#define T_BCK_START_NS 1000U
#define T_S_RGB_NS     250U
#define T_H_RGB_NS     250U
#define T_W_BCK_NS     440U
#define T_W_GEN_NS     11000U
#define T_HH_GCK_NS    5000U

struct pin {
  volatile uint32_t *bsrr;
  uint32_t on, off;
};

#define PIN(port, n)                                                                      \
  {(volatile uint32_t *)(uintptr_t)(GPIO_BASE(port) + STM32_GPIO_BSRR_OFFSET), 1U << (n), \
   1U << ((n) + 16U)}

static const struct pin s_vst = PIN(PORT_B, 10);
static const struct pin s_vck = PIN(PORT_A, 4);
static const struct pin s_enb = PIN(PORT_A, 7);
static const struct pin s_xrst = PIN(PORT_B, 1);
static const struct pin s_hst = PIN(PORT_B, 0);
static const struct pin s_hck = PIN(PORT_A, 6);
static volatile uint32_t *const s_data =
    (volatile uint32_t *)(uintptr_t)(GPIO_BASE(PORT_E) + STM32_GPIO_BSRR_OFFSET);

/* R1 G1 B1 carry the first pixel of a pair, R2 G2 B2 the second */
static const uint8_t s_r_pin[2] = {0, 8};
static const uint8_t s_g_pin[2] = {2, 10};
static const uint8_t s_b_pin[2] = {4, 12};
#define DATA_MASK 0x1515U

static uint16_t s_lut[2][2][64]; /* [plane][pixel][color] */
static uint32_t s_msb[W / 2U];
static uint32_t s_lsb[W / 2U];
static bool s_ready;

struct timing {
  uint32_t hs_intb, hs_gsp, gck, ls_gsp, hs_gck, bsp, bck_start, s_rgb, h_rgb, w_bck, w_gen, hh_gck;
};

static inline __attribute__((always_inline)) void prv_spin(uint32_t since, uint32_t n) {
  while ((cycles() - since) < n) {
  }
}

static inline __attribute__((always_inline)) void prv_wait(uint32_t n) {
  prv_spin(cycles(), n);
}

static inline __attribute__((always_inline)) void prv_on(const struct pin *p) {
  *p->bsrr = p->on;
}

static inline __attribute__((always_inline)) void prv_off(const struct pin *p) {
  *p->bsrr = p->off;
}

static void prv_timing(struct timing *t) {
  const uint32_t mhz = g_clock.sysclk_hz / 1000000U;
#define CYC(ns) ((((ns) * mhz) + 999U) / 1000U)
  *t = (struct timing){CYC(T_HS_INTB_NS), CYC(T_HS_GSP_NS), CYC(T_GCK_NS),       CYC(T_LS_GSP_NS),
                       CYC(T_HS_GCK_NS),  CYC(T_BSP_NS),    CYC(T_BCK_START_NS), CYC(T_S_RGB_NS),
                       CYC(T_H_RGB_NS),   CYC(T_W_BCK_NS),  CYC(T_W_GEN_NS),     CYC(T_HH_GCK_NS)};
#undef CYC
}

static void prv_lut_init(void) {
  for (unsigned plane = 0U; plane < 2U; plane++) {
    for (unsigned px = 0U; px < 2U; px++) {
      for (unsigned c = 0U; c < 64U; c++) {
        unsigned r = (c >> (5U - plane)) & 1U;
        unsigned g = (c >> (3U - plane)) & 1U;
        unsigned b = (c >> (1U - plane)) & 1U;
        s_lut[plane][px][c] =
            (uint16_t)((r << s_r_pin[px]) | (g << s_g_pin[px]) | (b << s_b_pin[px]));
      }
    }
  }
}

/* Panel line l: column 239 - l of the image, from the top */
static void prv_convert(const uint8_t *fb, unsigned line) {
  const uint8_t *src = fb == NULL ? NULL : &fb[(W - 1U) - line];

  for (unsigned k = 0U; k < W / 2U; k++) {
    uint8_t p0 = 0U, p1 = 0U;
    if (src != NULL) {
      p0 = src[(2U * k) * W] & 0x3fU;
      p1 = src[(2U * k + 1U) * W] & 0x3fU;
    }
    uint32_t m = (uint32_t)s_lut[0][0][p0] | s_lut[0][1][p1];
    uint32_t l = (uint32_t)s_lut[1][0][p0] | s_lut[1][1][p1];
    s_msb[k] = m | ((m ^ DATA_MASK) << 16);
    s_lsb[k] = l | ((l ^ DATA_MASK) << 16);
  }
}

static void prv_plane(const struct timing *t, const uint32_t *plane) {
  prv_wait(t->hs_gck);
  prv_on(&s_hst);
  prv_wait(t->bsp);
  prv_on(&s_hck);
  prv_wait(t->bck_start);
  prv_off(&s_hck);
  prv_wait(t->bsp);
  prv_off(&s_hst);
  prv_wait(t->bsp);

  for (unsigned k = 0U; k < W / 2U; k += 2U) {
    *s_data = plane[k];
    prv_wait(t->s_rgb);
    prv_on(&s_hck);
    prv_wait(t->h_rgb);
    *s_data = plane[k + 1U];
    prv_wait(t->s_rgb);
    prv_off(&s_hck);
    prv_wait(t->h_rgb);
  }

  prv_on(&s_hck);
  prv_on(&s_enb);
  prv_wait(t->w_bck);
  prv_off(&s_hck);
  prv_wait(t->w_gen);
  prv_off(&s_enb);
  prv_wait(t->hh_gck);
}

/* Every line, its MSB plane with GCK low and its LSB plane with GCK high */
static void prv_frame(const uint8_t *fb) {
  struct timing t;
  uint32_t edge;

  prv_timing(&t);
  prv_convert(fb, 0U);

  prv_on(&s_xrst);
  prv_wait(t.hs_intb);
  prv_on(&s_vst);
  prv_wait(t.hs_gsp);
  prv_on(&s_vck);
  prv_wait(t.gck);

  for (unsigned line = 0U; line < H; line++) {
    prv_off(&s_vck);
    edge = cycles();
    prv_plane(&t, s_msb);
    if (line == 0U) {
      prv_off(&s_vst);
      prv_wait(t.ls_gsp);
    }
    prv_spin(edge, t.gck);

    prv_on(&s_vck);
    edge = cycles();
    prv_plane(&t, s_lsb);
    if (line + 1U < H) {
      prv_convert(fb, line + 1U);
    }
    prv_spin(edge, t.gck);
  }

  prv_off(&s_vck);
  prv_wait(t.gck);
  prv_on(&s_vck);
  prv_wait(t.gck);
  prv_off(&s_vck);
  prv_off(&s_xrst);
  watchdog_feed();
}

static bool prv_lptim_write(uintptr_t base, uint32_t off, uint32_t value, uint32_t ok) {
  wr(base + STM32_LPTIM_ICR_OFFSET, ok);
  wr(base + off, value);
  uint32_t start = millis();
  while ((rd(base + STM32_LPTIM_ISR_OFFSET) & ok) == 0U) {
    if (millis() - start > 5U) {
      return false;
    }
  }
  return true;
}

/* 32768 / 1067 = 30.7 Hz, half the period high; LPTIM3 the complement */
static bool prv_vcom_setup(uintptr_t base, bool inverted) {
  wr(base + STM32_LPTIM_CR_OFFSET, 0U);
  wr(base + STM32_LPTIM_CFGR_OFFSET, 0U);
  wr(base + STM32_LPTIM_CCMR1_OFFSET,
     LPTIM_CCMR1_CCE(0U) | (inverted ? 0U : PBL_FIELD_PREP(LPTIM_CCMR1_CCP(0U), 1U)));
  wr(base + STM32_LPTIM_CR_OFFSET, LPTIM_CR_ENABLE);
  return prv_lptim_write(base, STM32_LPTIM_ARR_OFFSET, 1066U, LPTIM_ISR_ARROK) &&
         prv_lptim_write(base, STM32_LPTIM_CMP_OFFSET, 533U, LPTIM_ISR_CMPOK);
}

static bool prv_vcom_on(void) {
  uint32_t sel = g_clock.lse ? RCC_LPTIMSEL_LSE : RCC_LPTIMSEL_LSI;

  modb(RCC(STM32U5_RCC_CCIPR3_OFFSET), RCC_CCIPR3_LPTIM1SEL_MASK | RCC_CCIPR3_LPTIM34SEL_MASK,
       PBL_FIELD_PREP(RCC_CCIPR3_LPTIM1SEL_MASK, sel) |
           PBL_FIELD_PREP(RCC_CCIPR3_LPTIM34SEL_MASK, sel));
  setb(RCC(STM32U5_RCC_APB3ENR_OFFSET), RCC_APB3_LPTIM1 | RCC_APB3_LPTIM3);
  (void)rd(RCC(STM32U5_RCC_APB3ENR_OFFSET));

  if (!prv_vcom_setup(LPTIM1_BASE, false) || !prv_vcom_setup(LPTIM3_BASE, true)) {
    return false;
  }
  gpio_config(PORT_B, 2, GPIO_AF, false, PULL_NONE, 1U);
  gpio_config(PORT_D, 14, GPIO_AF, false, PULL_NONE, 14U);
  /* Both from 0 together, so that they stay complementary */
  wr(LPTIM1_BASE + STM32_LPTIM_CR_OFFSET, LPTIM_CR_ENABLE | LPTIM_CR_CNTSTRT);
  wr(LPTIM3_BASE + STM32_LPTIM_CR_OFFSET, LPTIM_CR_ENABLE | LPTIM_CR_CNTSTRT);
  return true;
}

bool display_init(void) {
  static const uint8_t ctl[][2] = {{PORT_B, 10}, {PORT_A, 4}, {PORT_A, 7},
                                   {PORT_B, 1},  {PORT_B, 0}, {PORT_A, 6}};
  for (unsigned i = 0U; i < sizeof(ctl) / sizeof(ctl[0]); i++) {
    gpio_write(ctl[i][0], ctl[i][1], false);
    gpio_config(ctl[i][0], ctl[i][1], GPIO_OUT, false, PULL_NONE, 0U);
  }
  for (unsigned px = 0U; px < 2U; px++) {
    const uint8_t pins[3] = {s_r_pin[px], s_g_pin[px], s_b_pin[px]};
    for (unsigned i = 0U; i < 3U; i++) {
      gpio_write(PORT_E, pins[i], false);
      gpio_config(PORT_E, pins[i], GPIO_OUT, false, PULL_NONE, 0U);
    }
  }
  prv_lut_init();

  /* The VCOM the bootloader may have left running stops first */
  setb(RCC(STM32U5_RCC_APB3ENR_OFFSET), RCC_APB3_LPTIM1 | RCC_APB3_LPTIM3);
  (void)rd(RCC(STM32U5_RCC_APB3ENR_OFFSET));
  wr(LPTIM1_BASE + STM32_LPTIM_CR_OFFSET, 0U);
  wr(LPTIM3_BASE + STM32_LPTIM_CR_OFFSET, 0U);

  /* The panel keeps what it showed, or noise: black first */
  prv_frame(NULL);
  s_ready = prv_vcom_on();
  return s_ready;
}

void display_frame(void) {
  prv_frame(g_fb);
}

void display_clear(uint8_t color) {
  memset(g_fb, color, sizeof(g_fb));
}

int display_text_width(const char *s, int scale) {
  return (int)strlen(s) * 8 * scale;
}

void display_text(int x, int y, int scale, uint8_t fg, uint8_t bg, const char *s) {
  if (scale < 1) {
    scale = 1;
  }
  for (; *s != '\0'; s++, x += 8 * scale) {
    unsigned c = (unsigned char)*s;
    const uint8_t *glyph = s_font8x8[(c >= 0x20U && c < 0x80U) ? c - 0x20U : '?' - 0x20U];
    for (int gy = 0; gy < 8 * scale; gy++) {
      int py = y + gy;
      if (py < 0 || py >= (int)H) {
        continue;
      }
      uint8_t row = glyph[gy / scale];
      for (int gx = 0; gx < 8 * scale; gx++) {
        int px = x + gx;
        if (px < 0 || px >= (int)W) {
          continue;
        }
        g_fb[py * (int)W + px] = (row & (0x80U >> (gx / scale))) ? fg : bg;
      }
    }
  }
}
