/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/* proto.c's hooks on the watch: memory accesses and calls under the guard */

#include "port.h"

#include "regs.h"
#include "stub.h"
#include "tether_api.h"

#include <string.h>

#define SEND_TIMEOUT_MS  2000U
#define FLUSH_TIMEOUT_MS 500U

extern uint32_t g_boot_csr;
extern struct tether_fault g_boot_fault;
extern uint32_t g_blink_until;
void boot_image(uint32_t addr) __attribute__((noreturn));
void system_reset(void) __attribute__((noreturn));

uint32_t port_millis(void) {
  return millis();
}

void port_send(const uint8_t *data, size_t len) {
  const uint32_t start = millis();
  size_t off = 0U;

  while (off < len) {
    usb_poll();
    off += usb_write(data + off, len - off);
    watchdog_feed();
    if (off < len && millis() - start > SEND_TIMEOUT_MS) {
      usb_tx_abort();
      return;
    }
  }
}

static void prv_flush(void) {
  const uint32_t start = millis();

  while (!usb_tx_idle() && millis() - start < FLUSH_TIMEOUT_MS) {
    usb_poll();
  }
  delay_ms(20U);
}

void port_info(struct tether_info *info) {
  memcpy(info->magic, "UTS1", 4);
  info->proto_version = TETHER_PROTO_VERSION;
  info->info_size = sizeof(*info);
  strncpy(info->version, TETHER_VERSION, sizeof(info->version) - 1U);
  info->reset_csr = g_boot_csr;
  info->sysclk_hz = g_clock.sysclk_hz;
  info->hclk_hz = g_clock.sysclk_hz;
  for (unsigned i = 0U; i < 3U; i++) {
    info->uid[i] = rd(UID_BASE + 4U * i);
  }
  info->flash_kb = flash_size_kb();
  info->idcode = rd(DBGMCU_IDCODE);
  info->optr = rd(FLASHR(STM32U5_FLASH_OPTR_OFFSET));
  info->uptime_ms = millis();
  info->usb_speed = g_usb.speed;
  info->usb_state = g_usb.state;
  info->usb_error = g_usb.error;
  info->buttons = buttons_read();
  info->flags = (g_clock.pll ? TETHER_INFO_PLL : 0U) | (g_clock.hse ? TETHER_INFO_HSE : 0U) |
                (g_clock.lse ? TETHER_INFO_LSE : 0U) |
                (g_boot_fault.magic == TETHER_FAULT_MAGIC ? TETHER_INFO_BOOT_FAULT : 0U) |
                (port_boot_check(TETHER_SLOT_BASE) == TETHER_OK ? TETHER_INFO_SLOT_VALID : 0U) |
                (flash_supported() ? TETHER_INFO_FLASH_OK : 0U);
  info->flags |=
      (g_display_ok ? TETHER_INFO_DISPLAY : 0U) | (g_force_fs ? TETHER_INFO_USB_FS_FORCED : 0U);
  info->load_base = TETHER_LOAD_BASE;
  info->load_end = TETHER_LOAD_END;
  info->blob_stack_top = TETHER_BLOB_STACK_TOP;
  info->out_buf = (uint32_t)(uintptr_t)g_out;
  info->out_size = OUT_SIZE;
  info->out_len = g_out_len;
  info->api = (uint32_t)(uintptr_t)&g_api;
  info->slot_base = TETHER_SLOT_BASE;
  info->slot_end = 0x08000000U + flash_size_kb() * 1024U;
  info->max_data = TETHER_MAX_DATA;
  info->cmd_count = g_status.cmds;
  info->err_count = g_status.errors;
  info->bkp3r = rd(TAMP_BKP3R);
  info->boot_fault = g_boot_fault;
  info->last_fault = g_last_fault;
}

struct memop {
  uint32_t addr;
  uint8_t *buf;
  uint32_t len;
  uint32_t width;
  uint32_t value;
  uint32_t crc;
};

static uint32_t prv_do_read(uint32_t p, uint32_t a1, uint32_t a2, uint32_t a3) {
  (void)a1, (void)a2, (void)a3;
  struct memop *m = (struct memop *)(uintptr_t)p;
  uint32_t i = 0U;

  if (m->width == 4U || (m->width == 0U && (m->addr % 4U) == 0U)) {
    for (; i + 4U <= m->len; i += 4U) {
      uint32_t v = *(volatile const uint32_t *)(uintptr_t)(m->addr + i);
      memcpy(&m->buf[i], &v, 4U);
    }
  } else if (m->width == 2U) {
    for (; i + 2U <= m->len; i += 2U) {
      uint16_t v = *(volatile const uint16_t *)(uintptr_t)(m->addr + i);
      memcpy(&m->buf[i], &v, 2U);
    }
  }
  for (; i < m->len; i++) {
    m->buf[i] = *(volatile const uint8_t *)(uintptr_t)(m->addr + i);
  }
  __asm volatile("dsb\n isb" ::: "memory");
  return 0U;
}

static uint32_t prv_do_write(uint32_t p, uint32_t a1, uint32_t a2, uint32_t a3) {
  (void)a1, (void)a2, (void)a3;
  struct memop *m = (struct memop *)(uintptr_t)p;
  uint32_t i = 0U;

  if (m->width == 4U || (m->width == 0U && (m->addr % 4U) == 0U)) {
    for (; i + 4U <= m->len; i += 4U) {
      uint32_t v;
      memcpy(&v, &m->buf[i], 4U);
      *(volatile uint32_t *)(uintptr_t)(m->addr + i) = v;
    }
  } else if (m->width == 2U) {
    for (; i + 2U <= m->len; i += 2U) {
      uint16_t v;
      memcpy(&v, &m->buf[i], 2U);
      *(volatile uint16_t *)(uintptr_t)(m->addr + i) = v;
    }
  }
  for (; i < m->len; i++) {
    *(volatile uint8_t *)(uintptr_t)(m->addr + i) = m->buf[i];
  }
  /* A write that faults does so before the guard ends */
  __asm volatile("dsb\n isb" ::: "memory");
  return 0U;
}

static uint32_t prv_do_fill(uint32_t p, uint32_t a1, uint32_t a2, uint32_t a3) {
  (void)a1, (void)a2, (void)a3;
  struct memop *m = (struct memop *)(uintptr_t)p;

  for (uint32_t i = 0U; i < m->len; i++) {
    if (m->width == 4U) {
      ((volatile uint32_t *)(uintptr_t)m->addr)[i] = m->value;
    } else if (m->width == 2U) {
      ((volatile uint16_t *)(uintptr_t)m->addr)[i] = (uint16_t)m->value;
    } else {
      ((volatile uint8_t *)(uintptr_t)m->addr)[i] = (uint8_t)m->value;
    }
  }
  __asm volatile("dsb\n isb" ::: "memory");
  return 0U;
}

static uint32_t prv_do_crc(uint32_t p, uint32_t a1, uint32_t a2, uint32_t a3) {
  (void)a1, (void)a2, (void)a3;
  struct memop *m = (struct memop *)(uintptr_t)p;
  uint8_t chunk[256];
  uint32_t crc = 0U;

  for (uint32_t off = 0U; off < m->len; off += sizeof(chunk)) {
    uint32_t n = m->len - off < sizeof(chunk) ? m->len - off : sizeof(chunk);
    struct memop r = {m->addr + off, chunk, n, 0U, 0U, 0U};
    (void)prv_do_read((uint32_t)(uintptr_t)&r, 0U, 0U, 0U);
    crc = tether_crc32(crc, chunk, n);
  }
  m->crc = crc;
  return 0U;
}

static int prv_guarded(uint32_t (*fn)(uint32_t, uint32_t, uint32_t, uint32_t), struct memop *m,
                       uint32_t timeout_ms, struct tether_fault *fault) {
  const uint32_t args[4] = {(uint32_t)(uintptr_t)m, 0U, 0U, 0U};
  uint32_t ret[2];

  g_guard.start_ms = millis();
  g_guard.timeout_ms = timeout_ms;
  int rc = guard_run(fn, args, TETHER_BLOB_STACK_TOP, ret);
  g_guard.timeout_ms = 0U;
  if (rc != TETHER_OK) {
    *fault = g_last_fault;
  }
  return rc;
}

int port_read(uint32_t addr, uint8_t *dst, uint32_t len, unsigned width,
              struct tether_fault *fault) {
  struct memop m = {addr, dst, len, width, 0U, 0U};
  return prv_guarded(prv_do_read, &m, 1000U, fault);
}

int port_write(uint32_t addr, const uint8_t *src, uint32_t len, unsigned width,
               struct tether_fault *fault) {
  struct memop m = {addr, (uint8_t *)src, len, width, 0U, 0U};
  return prv_guarded(prv_do_write, &m, 1000U, fault);
}

int port_fill(uint32_t addr, unsigned width, uint32_t value, uint32_t count,
              struct tether_fault *fault) {
  struct memop m = {addr, NULL, count, width, value, 0U};
  return prv_guarded(prv_do_fill, &m, 5000U, fault);
}

int port_crc32(uint32_t addr, uint32_t len, uint32_t *crc, struct tether_fault *fault) {
  struct memop m = {addr, NULL, len, 0U, 0U, 0U};
  int rc = prv_guarded(prv_do_crc, &m, 10000U, fault);
  *crc = m.crc;
  return rc;
}

int port_call(uint32_t addr, uint32_t flags, uint32_t timeout_ms, const uint32_t *args,
              uint32_t nargs, struct tether_call_result *res, struct tether_fault *fault) {
  static uint32_t s_args[TETHER_CALL_MAX_ARGS];
  uint32_t a[4] = {0U, 0U, 0U, 0U};
  uint32_t ret[2] = {0U, 0U};

  memcpy(s_args, args, nargs * sizeof(uint32_t));
  if (flags & TETHER_CALL_BLOB) {
    a[0] = (uint32_t)(uintptr_t)&g_api;
    a[1] = (uint32_t)(uintptr_t)s_args;
    a[2] = nargs;
  } else {
    memcpy(a, args, (nargs < 4U ? nargs : 4U) * sizeof(uint32_t));
  }
  if ((flags & TETHER_CALL_KEEP_OUT) == 0U) {
    g_out_len = 0U;
  }
  /* What the last blob drew stays up to the next call */
  g_status.blob_owns_display = false;

  g_guard.start_ms = millis();
  g_guard.timeout_ms = timeout_ms;
  const uint32_t t0 = cycles();
  int rc = guard_run((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))(uintptr_t)addr, a,
                     TETHER_BLOB_STACK_TOP, ret);
  const uint32_t dt = cycles() - t0;
  const uint32_t ms = millis() - g_guard.start_ms;
  g_guard.timeout_ms = 0U;
  irq_cleanup();

  res->r0 = ret[0];
  res->r1 = ret[1];
  res->elapsed_us = ms > 20000U ? ms * 1000U : dt / (g_clock.sysclk_hz / 1000000U);
  res->out_addr = (uint32_t)(uintptr_t)g_out;
  if (rc != TETHER_OK) {
    *fault = g_last_fault;
  }
  return rc;
}

const uint8_t *port_out(uint32_t *len) {
  *len = g_out_len;
  return (const uint8_t *)g_out;
}

int port_i2c(int bus, uint8_t addr, const uint8_t *wrb, uint32_t wn, uint8_t *rdb, uint32_t rn,
             bool fast) {
  return i2c_xfer(bus, addr, wrb, wn, rdb, rn, fast);
}

int port_gpio(unsigned port, unsigned pin, unsigned op, unsigned arg, uint32_t regs[8],
              uint8_t *level) {
  const uintptr_t base = GPIO_BASE(port);

  switch (op) {
    case TETHER_GPIO_OUT:
    case TETHER_GPIO_OUT_OD:
      gpio_write(port, pin, arg != 0U);
      gpio_config(port, pin, GPIO_OUT, op == TETHER_GPIO_OUT_OD, PULL_NONE, 0U);
      break;
    case TETHER_GPIO_IN:
      if (arg > PULL_DOWN) {
        return TETHER_ERR_ARG;
      }
      gpio_config(port, pin, GPIO_IN, false, arg, 0U);
      break;
    case TETHER_GPIO_ANALOG:
      gpio_config(port, pin, GPIO_ANALOG, false, PULL_NONE, 0U);
      break;
    default:
      break;
  }
  static const uint8_t s_offs[8] = {0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x20, 0x24};
  for (unsigned i = 0U; i < 8U; i++) {
    regs[i] = rd(base + s_offs[i]);
  }
  *level = gpio_read(port, pin) ? 1U : 0U;
  return TETHER_OK;
}

int port_flash_erase(uint32_t addr, uint32_t len) {
  fmt_snprintf(g_status.last_cmd, sizeof(g_status.last_cmd), "ERASE %08lx", (unsigned long)addr);
  return flash_erase(addr, len);
}

int port_flash_write(uint32_t addr, const uint8_t *data, uint32_t len) {
  return flash_program(addr, data, len);
}

int port_boot_check(uint32_t addr) {
  const uint32_t end = 0x08000000U + flash_size_kb() * 1024U;
  struct tether_fault fault;
  uint32_t vec[2];

  if (addr < TETHER_SLOT_BASE || addr + 8U > end || (addr % 1024U) != 0U) {
    return TETHER_ERR_PROTECTED;
  }
  if (port_read(addr, (uint8_t *)vec, sizeof(vec), 4U, &fault) != TETHER_OK) {
    return TETHER_ERR_FAULT;
  }
  if (vec[0] < 0x20000000U || vec[0] > 0x20270000U || (vec[1] & 1U) == 0U ||
      (vec[1] & ~1U) < addr || (vec[1] & ~1U) >= end) {
    return TETHER_ERR_ARG;
  }
  return TETHER_OK;
}

void port_note(const char *text) {
  strncpy(g_status.note, text, sizeof(g_status.note) - 1U);
  g_status.note[sizeof(g_status.note) - 1U] = '\0';
  g_status.blob_owns_display = false;
  status_changed();
}

void port_clear_fault(void) {
  memset(&g_last_fault, 0, sizeof(g_last_fault));
  memset(&g_boot_fault, 0, sizeof(g_boot_fault));
  status_changed();
}

void port_activity(uint8_t cmd, int status, uint32_t addr, uint32_t len) {
  static const char *const s_names[] = {
    "?",    "INFO",  "READ",  "WRITE", "FILL", "CRC",  "RUN",   "I2C",
    "GPIO", "RESET", "ERASE", "FLASH", "BOOT", "NOTE", "CLEAR",
  };
  const char *name = cmd < sizeof(s_names) / sizeof(s_names[0]) ? s_names[cmd] : "BAD";

  g_status.cmds++;
  if (status != TETHER_OK) {
    g_status.errors++;
  }
  if (cmd == TETHER_CMD_I2C) {
    fmt_snprintf(g_status.last_cmd, sizeof(g_status.last_cmd), "I2C %02lx %s", (unsigned long)addr,
                 status == TETHER_OK ? "ok" : "err");
  } else if (cmd == TETHER_CMD_INFO || cmd == TETHER_CMD_NOTE || cmd == TETHER_CMD_CLEAR_FAULT) {
    fmt_snprintf(g_status.last_cmd, sizeof(g_status.last_cmd), "%s", name);
  } else {
    fmt_snprintf(g_status.last_cmd, sizeof(g_status.last_cmd), "%s %08lx%s", name,
                 (unsigned long)addr, status == TETHER_OK ? "" : " !");
  }
  (void)len;
  g_blink_until = millis() + 40U;
  status_changed();
}

void port_action(enum port_action action, uint32_t arg) {
  prv_flush();
  switch (action) {
    case PORT_ACTION_POWER_OFF:
      /* The bootloader powers off after the reset, unless the power button is held */
      setb(PWR(STM32U5_PWR_DBPR_OFFSET), PWR_DBPR_DBP);
      wr(TAMP_BKP3R, 1U);
      system_reset();
    case PORT_ACTION_RESET:
      system_reset();
    case PORT_ACTION_BOOT:
      boot_image(arg);
    default:
      break;
  }
}
