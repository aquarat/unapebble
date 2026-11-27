/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The tether stub's protocol under PebbleOS, on the second USB serial port: proto.c of the stub,
// built from tools/una/tether/stub, with this port under it. Memory is read and written through
// probes whose faults are caught (pbl_fault_fixup()), I2C goes through the bus drivers, GPIO to
// the registers as the stub does. Calls of code and flashing are the stub's alone.

#include "dev_usb_private.h"

#include "port.h"
#include "tether_proto.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <pbl/device.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/i2c.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/fatal.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/mmio.h>
#include <pbl/kernel/sem.h>
#include <pbl/kernel/thread.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/usb/cdc_acm.h>
#include <pbl/usb/usbd.h>
#include <pbl/util/misc.h>

#include <system/version.h>

PBL_LOG_MODULE_DECLARE(dev_usb, CONFIG_DEV_USB_LOG_LEVEL);

#define PORT USB_CDC_ACM_UART(1)

#define RX_RING          8192U
#define RX_CHUNK         256U
#define POLL_MS          100U
#define SEND_TIMEOUT_MS  2000U
#define FLUSH_TIMEOUT_MS 500U
// A chunk of a CRC or a fill, between which other threads run
#define CHUNK 4096U

#define STACK_SIZE 3072U

// The chip's identity, as the stub reports it
#define UID_BASE       0x0bfa0700U
#define FLASHSIZE_BASE 0x0bfa07a0U
#define DBGMCU_IDCODE  0xe0044000U
#define FLASH_OPTR     0x40022040U
#define RCC_CSR        0x46020cf4U
#define TAMP_BKP3R     0x46007d0cU
#define GPIO_PORTS     9U

// The system control block and the MPU, which a write could turn against the system
#define SCB_START    0xe000ed00U
#define SCB_END      0xe000ee00U
#define SCB_NS_START 0xe002ed00U
#define SCB_NS_END   0xe002ee00U

#define CFSR_IMPRECISERR (1UL << 10)
#define CFSR_BFARVALID   (1UL << 15)
#define CFSR_MMARVALID   (1UL << 7)
#define SCB_BFAR         0xe000ed38U
#define SCB_MMFAR        0xe000ed34U
#define SCB_HFSR         0xe000ed2cU

static uint8_t s_rx[RX_RING];
static atomic_uint s_rx_head;
static atomic_uint s_rx_tail;
static PBL_SEM_DEFINE(s_rx_sem, 0, 1);

PBL_THREAD_STACK_DEFINE(s_stack, STACK_SIZE);
static struct pbl_thread s_thread;
static atomic_bool s_started;

static uint32_t s_cmds;
static uint32_t s_errors;
static struct tether_fault s_last_fault;

// Probes: one access each, a 16-bit instruction, whose fault the fixup steps over
uint32_t dev_usb_probe_ld8(uintptr_t addr);
uint32_t dev_usb_probe_ld16(uintptr_t addr);
uint32_t dev_usb_probe_ld32(uintptr_t addr);
void dev_usb_probe_st8(uintptr_t addr, uint32_t v);
void dev_usb_probe_st16(uintptr_t addr, uint32_t v);
void dev_usb_probe_st32(uintptr_t addr, uint32_t v);
extern const char dev_usb_probe_start[];
extern const char dev_usb_probe_end[];

__asm(
    "  .pushsection .text.dev_usb_probe, \"ax\", %progbits\n"
    "  .syntax unified\n"
    "  .thumb\n"
    "  .balign 4\n"
    "  .global dev_usb_probe_start\n"
    "dev_usb_probe_start:\n"
    "  .global dev_usb_probe_ld8\n"
    "  .type dev_usb_probe_ld8, %function\n"
    "  .thumb_func\n"
    "dev_usb_probe_ld8:\n"
    "  ldrb r0, [r0]\n"
    "  bx lr\n"
    "  .global dev_usb_probe_ld16\n"
    "  .type dev_usb_probe_ld16, %function\n"
    "  .thumb_func\n"
    "dev_usb_probe_ld16:\n"
    "  ldrh r0, [r0]\n"
    "  bx lr\n"
    "  .global dev_usb_probe_ld32\n"
    "  .type dev_usb_probe_ld32, %function\n"
    "  .thumb_func\n"
    "dev_usb_probe_ld32:\n"
    "  ldr r0, [r0]\n"
    "  bx lr\n"
    "  .global dev_usb_probe_st8\n"
    "  .type dev_usb_probe_st8, %function\n"
    "  .thumb_func\n"
    "dev_usb_probe_st8:\n"
    "  strb r1, [r0]\n"
    "  dsb sy\n"
    "  bx lr\n"
    "  .global dev_usb_probe_st16\n"
    "  .type dev_usb_probe_st16, %function\n"
    "  .thumb_func\n"
    "dev_usb_probe_st16:\n"
    "  strh r1, [r0]\n"
    "  dsb sy\n"
    "  bx lr\n"
    "  .global dev_usb_probe_st32\n"
    "  .type dev_usb_probe_st32, %function\n"
    "  .thumb_func\n"
    "dev_usb_probe_st32:\n"
    "  str r1, [r0]\n"
    "  dsb sy\n"
    "  bx lr\n"
    "  .global dev_usb_probe_end\n"
    "dev_usb_probe_end:\n"
    "  .popsection\n");

// Set by the fixup, read by the thread that probes: only the tether thread probes
static volatile bool s_probe_faulted;
static struct tether_fault s_probe_fault;

bool pbl_fault_fixup(const struct pbl_fault_info *info, uint32_t *frame, uint32_t status) {
  const uintptr_t pc = frame[6] & ~1U;
  struct tether_fault *f = &s_probe_fault;

  if (pc < (uintptr_t)dev_usb_probe_start || pc >= (uintptr_t)dev_usb_probe_end) {
    return false;
  }

  memset(f, 0, sizeof(*f));
  f->magic = TETHER_FAULT_MAGIC;
  f->exc = info->reason == PBL_FAULT_BUS ? 5U : info->reason == PBL_FAULT_MEMORY ? 4U : 3U;
  f->cfsr = status;
  f->hfsr = pbl_readl(SCB_HFSR);
  f->bfar = (status & CFSR_BFARVALID) != 0U ? pbl_readl(SCB_BFAR) : 0U;
  f->mmfar = (status & CFSR_MMARVALID) != 0U ? pbl_readl(SCB_MMFAR) : 0U;
  f->r0 = frame[0];
  f->r1 = frame[1];
  f->r2 = frame[2];
  f->r3 = frame[3];
  f->r12 = frame[4];
  f->lr = frame[5];
  f->pc = frame[6];
  f->xpsr = frame[7];
  f->sp = (uint32_t)(uintptr_t)&frame[8];
  f->uptime_ms = (uint32_t)pbl_uptime_ms();
  f->count = ++s_last_fault.count;

  // A buffered write faults later, at the barrier after it: nothing to step over
  if ((status & CFSR_IMPRECISERR) == 0U) {
    frame[6] += 2U;
    frame[0] = 0U;
  }
  s_probe_faulted = true;

  return true;
}

static int prv_probe_done(struct tether_fault *fault) {
  if (!s_probe_faulted) {
    return TETHER_OK;
  }

  s_probe_faulted = false;
  s_last_fault = s_probe_fault;
  if (fault != nullptr) {
    *fault = s_probe_fault;
  }

  return TETHER_ERR_FAULT;
}

static uint32_t prv_ld(uintptr_t a, unsigned width) {
  switch (width) {
    case 4U:
      return dev_usb_probe_ld32(a);
    case 2U:
      return dev_usb_probe_ld16(a);
    default:
      return dev_usb_probe_ld8(a);
  }
}

static void prv_st(uintptr_t a, unsigned width, uint32_t v) {
  switch (width) {
    case 4U:
      dev_usb_probe_st32(a, v);
      break;
    case 2U:
      dev_usb_probe_st16(a, v);
      break;
    default:
      dev_usb_probe_st8(a, v);
      break;
  }
}

// Width 0: words where aligned, as the stub reads; stops at the first fault
static int prv_read(uint32_t addr, uint8_t *dst, uint32_t len, unsigned width,
                    struct tether_fault *fault) {
  uint32_t i = 0U;

  s_probe_faulted = false;
  if (width == 4U || (width == 0U && (addr % 4U) == 0U)) {
    for (; i + 4U <= len && !s_probe_faulted; i += 4U) {
      uint32_t v = prv_ld(addr + i, 4U);
      memcpy(&dst[i], &v, 4U);
    }
  } else if (width == 2U) {
    for (; i + 2U <= len && !s_probe_faulted; i += 2U) {
      uint16_t v = (uint16_t)prv_ld(addr + i, 2U);
      memcpy(&dst[i], &v, 2U);
    }
  }
  for (; i < len && !s_probe_faulted; i++) {
    dst[i] = (uint8_t)prv_ld(addr + i, 1U);
  }

  return prv_probe_done(fault);
}

int port_read(uint32_t addr, uint8_t *dst, uint32_t len, unsigned width,
              struct tether_fault *fault) {
  return prv_read(addr, dst, len, width, fault);
}

int port_write(uint32_t addr, const uint8_t *src, uint32_t len, unsigned width,
               struct tether_fault *fault) {
  uint32_t i = 0U;

  s_probe_faulted = false;
  if (width == 4U || (width == 0U && (addr % 4U) == 0U)) {
    for (; i + 4U <= len && !s_probe_faulted; i += 4U) {
      uint32_t v;
      memcpy(&v, &src[i], 4U);
      prv_st(addr + i, 4U, v);
    }
  } else if (width == 2U) {
    for (; i + 2U <= len && !s_probe_faulted; i += 2U) {
      uint16_t v;
      memcpy(&v, &src[i], 2U);
      prv_st(addr + i, 2U, v);
    }
  }
  for (; i < len && !s_probe_faulted; i++) {
    prv_st(addr + i, 1U, src[i]);
  }

  return prv_probe_done(fault);
}

int port_fill(uint32_t addr, unsigned width, uint32_t value, uint32_t count,
              struct tether_fault *fault) {
  s_probe_faulted = false;
  for (uint32_t i = 0U; i < count && !s_probe_faulted; i++) {
    prv_st(addr + i * width, width, value);
    if ((i % CHUNK) == CHUNK - 1U) {
      pbl_thread_yield();
    }
  }

  return prv_probe_done(fault);
}

int port_crc32(uint32_t addr, uint32_t len, uint32_t *crc, struct tether_fault *fault) {
  static uint8_t s_chunk[256];
  uint32_t c = 0U;
  int ret;

  for (uint32_t off = 0U; off < len; off += sizeof(s_chunk)) {
    uint32_t n = len - off < sizeof(s_chunk) ? len - off : sizeof(s_chunk);

    ret = prv_read(addr + off, s_chunk, n, 0U, fault);
    if (ret != TETHER_OK) {
      return ret;
    }
    c = tether_crc32(c, s_chunk, n);
    if ((off % CHUNK) == 0U) {
      pbl_thread_yield();
    }
  }
  *crc = c;

  return TETHER_OK;
}

int port_call(uint32_t addr, uint32_t flags, uint32_t timeout_ms, const uint32_t *args,
              uint32_t nargs, struct tether_call_result *res, struct tether_fault *fault) {
  return TETHER_ERR_UNSUPPORTED;
}

const uint8_t *port_out(uint32_t *len) {
  *len = 0U;
  return nullptr;
}

static int prv_i2c_status(int ret) {
  switch (ret) {
    case 0:
      return TETHER_OK;
    case -ENXIO:
      return TETHER_ERR_NACK;
    case -ETIMEDOUT:
      return TETHER_ERR_TIMEOUT;
    default:
      return TETHER_ERR_BUS;
  }
}

// The bus driver's speed, whatever the request asks
int port_i2c(int bus, uint8_t addr, const uint8_t *wrb, uint32_t wn, uint8_t *rdb, uint32_t rn,
             bool fast) {
  static uint8_t s_wbuf[TETHER_I2C_MAX];
  struct pbl_i2c_msg msgs[2];
  uint8_t num = 0U;
  char name[8];
  const struct pbl_device *dev;

  (void)snprintf(name, sizeof(name), "i2c%d", bus);
  dev = pbl_device_find(&pbl_i2c_bus_class, name);
  if (dev == nullptr || pbl_device_init(dev) != 0) {
    return TETHER_ERR_ARG;
  }

  struct pbl_i2c_dev d = PBL_I2C_DEV(PBL_CONTAINER_OF(dev, const struct pbl_i2c_bus, dev), addr);

  memcpy(s_wbuf, wrb, wn);
  if (wn != 0U || rn == 0U) {
    msgs[num++] = (struct pbl_i2c_msg){.buf = s_wbuf, .len = wn, .flags = PBL_I2C_MSG_WRITE};
  }
  if (rn != 0U) {
    msgs[num++] = (struct pbl_i2c_msg){.buf = rdb, .len = rn, .flags = PBL_I2C_MSG_READ};
  }

  return prv_i2c_status(pbl_i2c_transfer(&d, msgs, num));
}

static uintptr_t prv_gpio_base(unsigned port) {
  return 0x42020000U + 0x400U * port;
}

static void prv_modify(uintptr_t reg, uint32_t mask, uint32_t value) {
  pbl_writel((pbl_readl(reg) & ~mask) | (value & mask), reg);
}

// The registers, as the stub drives them: the port's driver keeps its clock on
int port_gpio(unsigned port, unsigned pin, unsigned op, unsigned arg, uint32_t regs[8],
              uint8_t *level) {
  static const uint8_t s_offs[8] = {0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x20, 0x24};
  const uintptr_t base = prv_gpio_base(port);
  const uint32_t two = 3U << (pin * 2U);
  char name[8];
  const struct pbl_device *dev;
  uint32_t mode = 0U;

  if (port >= GPIO_PORTS) {
    return TETHER_ERR_ARG;
  }
  (void)snprintf(name, sizeof(name), "gpio%c", 'a' + port);
  dev = pbl_device_find(&pbl_gpio_port_class, name);
  if (dev == nullptr || pbl_device_init(dev) != 0) {
    return TETHER_ERR_ARG;
  }

  pbl_irq_lock();
  switch (op) {
    case TETHER_GPIO_OUT:
    case TETHER_GPIO_OUT_OD:
      pbl_writel(arg != 0U ? (1U << pin) : (1U << (pin + 16U)), base + 0x18U);
      prv_modify(base + 0x04U, 1U << pin, op == TETHER_GPIO_OUT_OD ? (1U << pin) : 0U);
      prv_modify(base + 0x0cU, two, 0U);
      mode = 1U;
      break;
    case TETHER_GPIO_IN:
      if (arg > 2U) {
        pbl_irq_unlock();
        return TETHER_ERR_ARG;
      }
      prv_modify(base + 0x0cU, two, arg << (pin * 2U));
      mode = 0U;
      break;
    case TETHER_GPIO_ANALOG:
      prv_modify(base + 0x0cU, two, 0U);
      mode = 3U;
      break;
    default:
      break;
  }
  if (op != TETHER_GPIO_READ) {
    prv_modify(base + 0x00U, two, mode << (pin * 2U));
  }
  for (unsigned i = 0U; i < 8U; i++) {
    regs[i] = pbl_readl(base + s_offs[i]);
  }
  *level = (pbl_readl(base + 0x10U) & (1U << pin)) != 0U ? 1U : 0U;
  pbl_irq_unlock();

  return TETHER_OK;
}

// Flashing is the stub's: tether.py resets to it first
int port_flash_erase(uint32_t addr, uint32_t len) {
  return TETHER_ERR_UNSUPPORTED;
}

int port_flash_write(uint32_t addr, const uint8_t *data, uint32_t len) {
  return TETHER_ERR_UNSUPPORTED;
}

int port_boot_check(uint32_t addr) {
  return TETHER_ERR_UNSUPPORTED;
}

bool port_write_protected(uint32_t addr, uint32_t len) {
  return proto_overlaps(addr, len, SCB_START, SCB_END) ||
         proto_overlaps(addr, len, SCB_NS_START, SCB_NS_END);
}

void port_note(const char *text) {
  PBL_LOG_INFO("Tether note: %s", text);
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

uint32_t port_millis(void) {
  return (uint32_t)pbl_uptime_ms();
}

// Only to a host holding the port open; a reply nobody reads in time is dropped
void port_send(const uint8_t *data, size_t len) {
  const uint64_t start = pbl_uptime_ms();
  size_t off = 0U;

  while (off < len && pbl_usb_cdc_acm_dtr(PORT)) {
    off += pbl_usb_cdc_acm_write(PORT, &data[off], len - off);
    if (off < len) {
      if (pbl_uptime_ms() - start > SEND_TIMEOUT_MS) {
        return;
      }
      pbl_thread_sleep(PBL_MSEC(1));
    }
  }
}

static uint8_t prv_buttons(void) {
  // Back PD8, Up PD4, Select PD15, Down PA5, active low
  const uint32_t d = pbl_readl(prv_gpio_base(3U) + 0x10U);
  const uint32_t a = pbl_readl(prv_gpio_base(0U) + 0x10U);
  uint8_t b = 0U;

  b |= (d & (1U << 8)) != 0U ? 0U : 1U;
  b |= (d & (1U << 4)) != 0U ? 0U : 2U;
  b |= (d & (1U << 15)) != 0U ? 0U : 4U;
  b |= (a & (1U << 5)) != 0U ? 0U : 8U;
  return b;
}

void port_info(struct tether_info *info) {
  memcpy(info->magic, "UTS1", 4);
  info->proto_version = TETHER_PROTO_VERSION;
  info->info_size = sizeof(*info);
  (void)snprintf(info->version, sizeof(info->version), "PebbleOS %.22s",
                 TINTIN_METADATA.version_tag);
  info->reset_csr = pbl_readl(RCC_CSR);
  info->sysclk_hz = pbl_cycles_per_sec();
  info->hclk_hz = pbl_cycles_per_sec();
  for (unsigned i = 0U; i < 3U; i++) {
    info->uid[i] = pbl_readl(UID_BASE + 4U * i);
  }
  info->flash_kb = pbl_readl(FLASHSIZE_BASE) & 0xffffU;
  info->idcode = pbl_readl(DBGMCU_IDCODE);
  info->optr = pbl_readl(FLASH_OPTR);
  info->uptime_ms = (uint32_t)pbl_uptime_ms();
  info->usb_speed = pbl_usbd_bulk_mps() == 512U ? 2U : 1U;
  info->usb_state = pbl_usb_cdc_acm_dtr(PORT) ? TETHER_USB_OPEN : TETHER_USB_CONFIGURED;
  info->buttons = prv_buttons();
  info->flags = TETHER_INFO_PEBBLEOS;
#ifdef CONFIG_BOARD_UNAWATCH_TETHER_SLOT
  info->slot_base = TETHER_SLOT_BASE;
#endif
  info->slot_end = 0x08000000U + info->flash_kb * 1024U;
  info->max_data = TETHER_MAX_DATA;
  info->cmd_count = s_cmds;
  info->err_count = s_errors;
  info->bkp3r = pbl_readl(TAMP_BKP3R);
  info->last_fault = s_last_fault;
}

// Once the reply has gone: a restart from PebbleOS goes through the bootloader, to the stub when
// the image runs from its slot
void port_action(enum port_action action, uint32_t arg) {
  (void)PBL_POLL_SLEEP_MS(pbl_usb_cdc_acm_tx_idle(PORT), 5U, FLUSH_TIMEOUT_MS);

  switch (action) {
    case PORT_ACTION_RESET:
      dev_usb_reboot();
      break;
    case PORT_ACTION_POWER_OFF:
      dev_usb_power_off();
      break;
    default:
      break;
  }
}

// From the controller's interrupt: kept for the thread, dropped when it lags; the protocol resyncs
static void prv_rx(const struct pbl_uart *uart, uint8_t c, void *arg) {
  const unsigned head = atomic_load_explicit(&s_rx_head, memory_order_relaxed);

  if (head - atomic_load_explicit(&s_rx_tail, memory_order_acquire) < RX_RING) {
    s_rx[head % RX_RING] = c;
    atomic_store_explicit(&s_rx_head, head + 1U, memory_order_release);
  }
  pbl_sem_give(&s_rx_sem);
}

static void prv_thread(void *arg) {
  static uint8_t s_chunk[RX_CHUNK];

  for (;;) {
    (void)pbl_sem_take(&s_rx_sem, PBL_MSEC(POLL_MS));

    for (;;) {
      const unsigned tail = atomic_load_explicit(&s_rx_tail, memory_order_relaxed);
      const unsigned head = atomic_load_explicit(&s_rx_head, memory_order_acquire);
      unsigned n = head - tail;

      if (n == 0U) {
        break;
      }
      if (n > RX_CHUNK) {
        n = RX_CHUNK;
      }
      for (unsigned i = 0U; i < n; i++) {
        s_chunk[i] = s_rx[(tail + i) % RX_RING];
      }
      atomic_store_explicit(&s_rx_tail, tail + n, memory_order_release);
      proto_input(s_chunk, n);
    }
    proto_poll();
  }
}

void dev_usb_tether_start(void) {
  if (atomic_exchange(&s_started, true)) {
    return;
  }

  (void)pbl_uart_rx_enable(PORT, prv_rx, nullptr);

  const struct pbl_thread_attr attr = {
    .name = "Tether",
    .entry = prv_thread,
    .prio = CONFIG_DEV_USB_THREAD_PRIO,
    .privileged = true,
    .stack = s_stack,
    .stack_size = sizeof(s_stack),
  };
  if (pbl_thread_create(&s_thread, &attr) != 0) {
    PBL_LOG_ERR("No tether thread");
  }
}
