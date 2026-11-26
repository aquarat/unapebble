/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * What the stub gives a blob it runs: a blob's entry is called with a pointer to
 * this table, its arguments and their count, on its own stack, privileged, in
 * thread mode. Faults and timeouts return to the stub. Blobs must not change
 * CONTROL, the vector table the stub set, the clocks of the USB (HCLK above
 * 30 MHz, HSE) or the USB itself.
 */

#pragma once

#include <stdarg.h>
#include <stdint.h>

#define TETHER_API_MAGIC   0x49504154U /* "TAPI" */
#define TETHER_API_VERSION 1U

#define TETHER_FB_WIDTH  240U
#define TETHER_FB_HEIGHT 240U

/* ARGB2222 colors of the framebuffer, alpha ignored */
#define TETHER_BLACK  0x00U
#define TETHER_WHITE  0x3fU
#define TETHER_RED    0x30U
#define TETHER_GREEN  0x0cU
#define TETHER_BLUE   0x03U
#define TETHER_YELLOW 0x3cU

struct tether_api {
  uint32_t magic;
  uint32_t version;
  /* Into the output buffer the host gets back; %d %i %u %x %X %p %s %c, widths, l, ll */
  int (*printf)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
  int (*vprintf)(const char *fmt, va_list ap);
  void (*write)(const void *data, uint32_t len);
  void (*delay_us)(uint32_t us);
  uint32_t (*millis)(void);
  uint32_t (*cycles)(void);
  /* bus 1 to 4; returns 0 or a tether_status. Writes to the PMIC, the gauge and the
     USB-PD chip on bus 1 longer than a register address need unsafe set. */
  int (*i2c)(int bus, uint8_t addr, const uint8_t *wr, uint32_t wn, uint8_t *rd, uint32_t rn,
             int unsafe);
  void (*watchdog_feed)(void);
  /* 240 x 240 ARGB2222, upright; display_update sends it, about 35 ms */
  uint8_t *fb;
  void (*display_update)(void);
  /* Draws text into the framebuffer; scale 1 is 8 x 8 */
  void (*draw_text)(int x, int y, int scale, uint8_t fg, uint8_t bg, const char *s);
  /* Restores the stub's own screen on the next refresh */
  void (*display_release)(void);
  /* Installs an IRQ handler in the stub's vector table; the stub disables every IRQ and
     drops the handlers after the call */
  void (*set_irq_handler)(int irq, void (*handler)(void));
  uint32_t cpu_hz;
  uint32_t out_buf;
  uint32_t out_size;
};

/* At the start of a blob image, at its link address */
struct tether_blob_header {
  uint32_t magic; /* TETHER_BLOB_MAGIC */
  uint32_t version;
  uint32_t load_addr;
  uint32_t entry; /* uint32_t entry(const struct tether_api *, const uint32_t *args, uint32_t n) */
  uint32_t image_end;
  uint32_t bss_end;
  char name[24];
};
