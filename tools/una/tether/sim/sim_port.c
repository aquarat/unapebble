/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The stub's protocol (stub/proto.c, unchanged) on a host, over stdin and
 * stdout, with a simulated watch behind it: SRAM, the 4 MiB flash, a fuel
 * gauge and a PMIC on I2C1, GPIO ports. Anything else faults as a bus error.
 * Blobs cannot run here; a blob whose header names one of these behaviours
 * gets it: sim_echo prints its arguments and returns their sum, sim_big
 * prints 10000 bytes, sim_fault faults, sim_hang times out. With
 * TETHER_SIM_PEBBLEOS set it answers as PebbleOS's USB developer access does,
 * without calls or flashing, until a reset turns it into the stub.
 */

#include "port.h"
#include "tether_api.h"
#include "tether_proto.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <poll.h>
#include <unistd.h>

#define RAM_BASE   0x20000000U
#define RAM_SIZE   0x270000U
#define FLASH_BASE 0x08000000U
#define FLASH_SIZE 0x400000U
#define OUT_ADDR   0x20010000U
#define OUT_SIZE   16384U

static uint8_t *s_ram;
static uint8_t *s_flash;
static uint32_t s_out_len;
static struct tether_fault s_last_fault;
static uint32_t s_cmds, s_errors;
static char s_note[32];
static uint32_t s_gpio[9][8];
static uint16_t s_gauge[256];
static uint8_t s_gauge_ptr;
static uint8_t s_pmic[256];
static uint8_t s_pmic_ptr;
static bool s_pebbleos;

uint32_t port_millis(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000U + ts.tv_nsec / 1000000U);
}

void port_send(const uint8_t *data, size_t len) {
  while (len != 0U) {
    ssize_t n = write(STDOUT_FILENO, data, len);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      exit(1);
    }
    data += n;
    len -= (size_t)n;
  }
}

static uint8_t *prv_mem(uint32_t addr, uint32_t len, bool write) {
  if (addr >= RAM_BASE && len <= RAM_SIZE && addr - RAM_BASE <= RAM_SIZE - len) {
    return &s_ram[addr - RAM_BASE];
  }
  if (!write && addr >= FLASH_BASE && len <= FLASH_SIZE && addr - FLASH_BASE <= FLASH_SIZE - len) {
    return &s_flash[addr - FLASH_BASE];
  }
  return NULL;
}

static int prv_bus_fault(uint32_t addr, struct tether_fault *fault) {
  memset(fault, 0, sizeof(*fault));
  fault->magic = TETHER_FAULT_MAGIC;
  fault->exc = 5U;       /* BusFault */
  fault->cfsr = 0x8200U; /* BFARVALID | PRECISERR */
  fault->bfar = addr;
  fault->pc = 0x08060abcU;
  fault->count = ++s_last_fault.count;
  s_last_fault = *fault;
  return TETHER_ERR_FAULT;
}

void port_info(struct tether_info *info) {
  memcpy(info->magic, "UTS1", 4);
  info->proto_version = TETHER_PROTO_VERSION;
  info->info_size = sizeof(*info);
  strcpy(info->version, s_pebbleos ? "PebbleOS sim" : "sim");
  info->reset_csr = 0x0c000000U;
  info->sysclk_hz = info->hclk_hz = 160000000U;
  info->uid[0] = 0x11111111U;
  info->uid[1] = 0x22222222U;
  info->uid[2] = 0x33333333U;
  info->flash_kb = 4096U;
  info->idcode = 0x20016481U;
  info->optr = 0x1fef77aaU;
  info->uptime_ms = port_millis();
  info->usb_speed = 2U;
  info->usb_state = TETHER_USB_OPEN;
  info->flags = TETHER_INFO_PLL | TETHER_INFO_HSE | TETHER_INFO_LSE | TETHER_INFO_DISPLAY |
                TETHER_INFO_FLASH_OK |
                (port_boot_check(TETHER_SLOT_BASE) == TETHER_OK ? TETHER_INFO_SLOT_VALID : 0U);
  if (s_pebbleos) {
    info->flags = TETHER_INFO_PEBBLEOS;
  }
  info->load_base = TETHER_LOAD_BASE;
  info->load_end = TETHER_LOAD_END;
  info->blob_stack_top = TETHER_BLOB_STACK_TOP;
  info->out_buf = OUT_ADDR;
  info->out_size = OUT_SIZE;
  info->out_len = s_out_len;
  info->api = 0x20000100U;
  info->slot_base = TETHER_SLOT_BASE;
  info->slot_end = FLASH_BASE + FLASH_SIZE;
  info->max_data = TETHER_MAX_DATA;
  info->cmd_count = s_cmds;
  info->err_count = s_errors;
  info->last_fault = s_last_fault;
}

int port_read(uint32_t addr, uint8_t *dst, uint32_t len, unsigned width,
              struct tether_fault *fault) {
  const uint8_t *m = prv_mem(addr, len, false);
  if (m == NULL) {
    return prv_bus_fault(addr, fault);
  }
  memcpy(dst, m, len);
  return TETHER_OK;
}

int port_write(uint32_t addr, const uint8_t *src, uint32_t len, unsigned width,
               struct tether_fault *fault) {
  uint8_t *m = prv_mem(addr, len, true);
  if (m == NULL) {
    return prv_bus_fault(addr, fault);
  }
  memcpy(m, src, len);
  return TETHER_OK;
}

int port_fill(uint32_t addr, unsigned width, uint32_t value, uint32_t count,
              struct tether_fault *fault) {
  uint8_t *m = prv_mem(addr, count * width, true);
  if (m == NULL) {
    return prv_bus_fault(addr, fault);
  }
  for (uint32_t i = 0U; i < count; i++) {
    memcpy(&m[i * width], &value, width);
  }
  return TETHER_OK;
}

int port_crc32(uint32_t addr, uint32_t len, uint32_t *crc, struct tether_fault *fault) {
  const uint8_t *m = prv_mem(addr, len, false);
  if (m == NULL) {
    return prv_bus_fault(addr, fault);
  }
  *crc = tether_crc32(0U, m, len);
  return TETHER_OK;
}

static void prv_out(const char *s, size_t n) {
  if (n > OUT_SIZE - s_out_len) {
    n = OUT_SIZE - s_out_len;
  }
  memcpy(&s_ram[OUT_ADDR - RAM_BASE + s_out_len], s, n);
  s_out_len += (uint32_t)n;
}

int port_call(uint32_t addr, uint32_t flags, uint32_t timeout_ms, const uint32_t *args,
              uint32_t nargs, struct tether_call_result *res, struct tether_fault *fault) {
  const struct tether_blob_header *h =
      (const struct tether_blob_header *)&s_ram[TETHER_LOAD_BASE - RAM_BASE];
  char line[128];

  if (s_pebbleos) {
    return TETHER_ERR_UNSUPPORTED;
  }
  if ((flags & TETHER_CALL_KEEP_OUT) == 0U) {
    s_out_len = 0U;
  }
  res->out_addr = OUT_ADDR;
  if (h->magic != TETHER_BLOB_MAGIC || h->entry != addr) {
    return prv_bus_fault(addr & ~1U, fault);
  }
  if (strcmp(h->name, "sim_echo") == 0) {
    uint32_t sum = 0U;
    snprintf(line, sizeof(line), "echo %u args:", nargs);
    prv_out(line, strlen(line));
    for (uint32_t i = 0U; i < nargs; i++) {
      snprintf(line, sizeof(line), " %x", args[i]);
      prv_out(line, strlen(line));
      sum += args[i];
    }
    prv_out("\n", 1U);
    res->r0 = sum;
    res->r1 = (flags & TETHER_CALL_BLOB) ? 1U : 0U;
    res->elapsed_us = 42U;
    return TETHER_OK;
  }
  if (strcmp(h->name, "sim_big") == 0) {
    for (unsigned i = 0U; i < 10000U; i++) {
      char c = (char)('a' + (i % 26U));
      prv_out(&c, 1U);
    }
    res->r0 = 10000U;
    return TETHER_OK;
  }
  if (strcmp(h->name, "sim_hang") == 0) {
    memset(fault, 0, sizeof(*fault));
    fault->magic = TETHER_FAULT_MAGIC;
    fault->exc = TETHER_FAULT_TIMEOUT;
    fault->pc = addr & ~1U;
    s_last_fault = *fault;
    return TETHER_ERR_TIMEOUT;
  }
  prv_out("about to fault\n", 15U);
  int rc = prv_bus_fault(0xdeadbee0U, fault);
  fault->exc = 3U;
  fault->pc = (addr & ~1U) + 8U;
  s_last_fault = *fault;
  return rc;
}

const uint8_t *port_out(uint32_t *len) {
  *len = s_out_len;
  return &s_ram[OUT_ADDR - RAM_BASE];
}

int port_i2c(int bus, uint8_t addr, const uint8_t *wr, uint32_t wn, uint8_t *rd, uint32_t rn,
             bool fast) {
  if (bus < 1 || bus > 4) {
    return TETHER_ERR_ARG;
  }
  if (bus == 1 && addr == 0x36U) {
    if (wn >= 1U) {
      s_gauge_ptr = wr[0];
      for (uint32_t i = 1U; i + 1U < wn; i += 2U) {
        s_gauge[(uint8_t)(s_gauge_ptr + (i - 1U) / 2U)] = (uint16_t)(wr[i] | (wr[i + 1U] << 8));
      }
    }
    for (uint32_t i = 0U; i < rn; i++) {
      uint16_t v = s_gauge[(uint8_t)(s_gauge_ptr + i / 2U)];
      rd[i] = (uint8_t)(i % 2U ? v >> 8 : v);
    }
    return TETHER_OK;
  }
  if (bus == 1 && addr == 0x61U) {
    if (wn >= 1U) {
      s_pmic_ptr = wr[0];
      for (uint32_t i = 1U; i < wn; i++) {
        s_pmic[(uint8_t)(s_pmic_ptr + i - 1U)] = wr[i];
      }
    }
    for (uint32_t i = 0U; i < rn; i++) {
      rd[i] = s_pmic[(uint8_t)(s_pmic_ptr + i)];
    }
    return TETHER_OK;
  }
  return TETHER_ERR_NACK;
}

int port_gpio(unsigned port, unsigned pin, unsigned op, unsigned arg, uint32_t regs[8],
              uint8_t *level) {
  uint32_t *r = s_gpio[port];
  const uint32_t m2 = 3U << (pin * 2U);

  switch (op) {
    case TETHER_GPIO_OUT:
    case TETHER_GPIO_OUT_OD:
      r[0] = (r[0] & ~m2) | (1U << (pin * 2U));
      r[1] = op == TETHER_GPIO_OUT_OD ? (r[1] | (1U << pin)) : (r[1] & ~(1U << pin));
      r[5] = arg ? (r[5] | (1U << pin)) : (r[5] & ~(1U << pin));
      r[4] = r[5];
      break;
    case TETHER_GPIO_IN:
      if (arg > 2U) {
        return TETHER_ERR_ARG;
      }
      r[0] &= ~m2;
      r[3] = (r[3] & ~m2) | (arg << (pin * 2U));
      /* A pull-up reads high, anything else low */
      r[4] = arg == 1U ? (r[4] | (1U << pin)) : (r[4] & ~(1U << pin));
      break;
    case TETHER_GPIO_ANALOG:
      r[0] |= m2;
      break;
    default:
      break;
  }
  memcpy(regs, r, 8U * sizeof(uint32_t));
  *level = (r[4] >> pin) & 1U;
  return TETHER_OK;
}

static bool prv_in_slot(uint32_t addr, uint32_t len) {
  return addr >= TETHER_SLOT_BASE && addr <= FLASH_BASE + FLASH_SIZE &&
         len <= FLASH_BASE + FLASH_SIZE - addr;
}

int port_flash_erase(uint32_t addr, uint32_t len) {
  if (s_pebbleos) {
    return TETHER_ERR_UNSUPPORTED;
  }
  if (!prv_in_slot(addr, len) || addr % TETHER_FLASH_PAGE || len % TETHER_FLASH_PAGE) {
    return TETHER_ERR_PROTECTED;
  }
  memset(&s_flash[addr - FLASH_BASE], 0xff, len);
  return TETHER_OK;
}

int port_flash_write(uint32_t addr, const uint8_t *data, uint32_t len) {
  if (s_pebbleos) {
    return TETHER_ERR_UNSUPPORTED;
  }
  if (!prv_in_slot(addr, len) || addr % TETHER_FLASH_PROG_ALIGN || len % TETHER_FLASH_PROG_ALIGN) {
    return TETHER_ERR_PROTECTED;
  }
  uint8_t *m = &s_flash[addr - FLASH_BASE];
  for (uint32_t i = 0U; i < len; i++) {
    if (m[i] != 0xffU) {
      return TETHER_ERR_FLASH;
    }
  }
  memcpy(m, data, len);
  return TETHER_OK;
}

/* The stub's own RAM, and its alias */
bool port_write_protected(uint32_t addr, uint32_t len) {
  return proto_overlaps(addr, len, TETHER_STUB_RAM, TETHER_STUB_RAM + TETHER_STUB_RAM_SIZE) ||
         proto_overlaps(addr, len, 0x30000000U, 0x30000000U + TETHER_STUB_RAM_SIZE);
}

int port_boot_check(uint32_t addr) {
  uint32_t sp, pc;

  if (addr < TETHER_SLOT_BASE || addr + 8U > FLASH_BASE + FLASH_SIZE || addr % 1024U) {
    return TETHER_ERR_PROTECTED;
  }
  memcpy(&sp, &s_flash[addr - FLASH_BASE], 4U);
  memcpy(&pc, &s_flash[addr - FLASH_BASE + 4U], 4U);
  if (sp < 0x20000000U || sp > 0x20270000U || (pc & 1U) == 0U || (pc & ~1U) < addr ||
      (pc & ~1U) >= FLASH_BASE + FLASH_SIZE) {
    return TETHER_ERR_ARG;
  }
  return TETHER_OK;
}

void port_note(const char *text) {
  snprintf(s_note, sizeof(s_note), "%s", text);
  fprintf(stderr, "sim: note \"%s\"\n", s_note);
}

void port_clear_fault(void) {
  memset(&s_last_fault, 0, sizeof(s_last_fault));
}

void port_activity(uint8_t cmd, int status, uint32_t addr, uint32_t len) {
  s_cmds++;
  if (status != TETHER_OK) {
    s_errors++;
  }
}

void port_action(enum port_action action, uint32_t arg) {
  static const char *const s_names[] = {"none", "reset", "power off", "boot"};
  fprintf(stderr, "sim: %s %08x\n", s_names[action], arg);
  /* PebbleOS, a slot build, restarts into the stub */
  if (action == PORT_ACTION_RESET) {
    s_pebbleos = false;
  }
}

int main(void) {
  s_pebbleos = getenv("TETHER_SIM_PEBBLEOS") != NULL;
  s_ram = calloc(1U, RAM_SIZE);
  s_flash = malloc(FLASH_SIZE);
  if (s_ram == NULL || s_flash == NULL) {
    return 1;
  }
  memset(s_flash, 0xff, FLASH_SIZE);
  /* Something below the slot, which nothing may change */
  memcpy(s_flash, "UNA bootloader", 14U);
  s_gauge[0x06] = 50U * 256U; /* RepSOC 50 % */
  s_gauge[0x09] = 0xc800U;    /* VCell 4.0 V */
  s_gauge[0x21] = 0x4031U;    /* DevName */
  s_pmic[0x00] = 0x12U;

  for (;;) {
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    int r = poll(&pfd, 1, 50);
    if (r < 0 && errno != EINTR) {
      return 1;
    }
    if (r > 0) {
      uint8_t buf[4096];
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n == 0) {
        return 0;
      }
      if (n > 0) {
        proto_input(buf, (size_t)n);
      } else if (errno != EINTR && errno != EAGAIN) {
        return 1;
      }
    }
    proto_poll();
  }
}
