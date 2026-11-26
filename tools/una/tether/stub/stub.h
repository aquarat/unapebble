/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "tether_proto.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef TETHER_VERSION
#define TETHER_VERSION "dev"
#endif

/* clock.c */
struct clock_state {
  bool pll;
  bool hse;
  bool lse;
  uint32_t sysclk_hz;
};
extern struct clock_state g_clock;
extern volatile uint32_t g_ms;
void clock_init(void);
bool clock_hse_on(void);
uint32_t millis(void);
uint32_t cycles(void);
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);
void watchdog_feed(void);

/* gpio.c */
enum {
  GPIO_IN,
  GPIO_OUT,
  GPIO_AF,
  GPIO_ANALOG
};
enum {
  PULL_NONE,
  PULL_UP,
  PULL_DOWN
};
void gpio_clocks_on(void);
void gpio_config(unsigned port, unsigned pin, unsigned mode, bool open_drain, unsigned pull,
                 unsigned af);
void gpio_write(unsigned port, unsigned pin, bool level);
bool gpio_read(unsigned port, unsigned pin);
void gpio_pins_init(void);
uint8_t buttons_read(void);

/* usb.c */
struct usb_status {
  uint8_t state;
  uint8_t speed; /* 0 unknown, 1 FS, 2 HS */
  uint8_t error; /* the init step that failed, 0 none */
  bool dtr;
};
extern struct usb_status g_usb;
void usb_init(bool force_fs);
void usb_poll(void);
/* Takes what fits, returns how much */
size_t usb_write(const uint8_t *data, size_t len);
bool usb_tx_idle(void);
size_t usb_read(uint8_t *data, size_t len);
void usb_tx_abort(void);
void usb_shutdown(void);
/* No control request for that long: a display frame now delays none */
bool usb_quiet(uint32_t ms);

/* display.c */
extern uint8_t g_fb[240 * 240];
bool display_init(void);
void display_frame(void);
void display_clear(uint8_t color);
void display_text(int x, int y, int scale, uint8_t fg, uint8_t bg, const char *s);
int display_text_width(const char *s, int scale);

/* i2c.c */
int i2c_xfer(int bus, uint8_t addr, const uint8_t *wr, uint32_t wn, uint8_t *rd, uint32_t rn,
             bool fast);

/* flash.c */
bool flash_supported(void);
uint32_t flash_size_kb(void);
int flash_erase(uint32_t addr, uint32_t len);
int flash_program(uint32_t addr, const uint8_t *data, uint32_t len);
void icache_invalidate(void);

/* fmt.c */
int fmt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int fmt_snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* main.c: the output buffer of blobs, the guard and faults */
#define OUT_SIZE 16384U
extern char g_out[OUT_SIZE];
extern uint32_t g_out_len;
void out_write(const void *data, uint32_t len);
int out_vprintf(const char *fmt, va_list ap);
int out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

struct guard {
  uint32_t saved_msp; /* nonzero while a guarded call runs */
  uint32_t start_ms;
  uint32_t timeout_ms;
};
extern volatile struct guard g_guard;
extern struct tether_fault g_last_fault;
int guard_run(uint32_t (*fn)(uint32_t, uint32_t, uint32_t, uint32_t), const uint32_t args[4],
              uint32_t psp_top, uint32_t ret[2]);
void boot_jump(uint32_t sp, uint32_t pc) __attribute__((noreturn));

struct tether_api;
extern struct tether_api g_api;
void irq_cleanup(void);

/* The screen's state */
struct status {
  char last_cmd[24];
  char note[32];
  uint32_t cmds;
  uint32_t errors;
  bool dirty;
  bool blob_owns_display;
};
extern struct status g_status;
extern bool g_display_ok;
extern bool g_force_fs;
void status_changed(void);
