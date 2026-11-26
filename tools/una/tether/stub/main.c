/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The tether stub: started by the UNA bootloader at 0x08060000, it brings up
 * the clocks, the display and a USB CDC-ACM port, and runs the commands of
 * tools/una/tether/tether.py. Polled throughout; SysTick only counts time and
 * stops blobs that overrun.
 */

#include "port.h"
#include "regs.h"
#include "stub.h"
#include "tether_api.h"

#include <string.h>

#define IRQ_WORDS ((NUM_IRQS + 31U) / 32U)

extern const uint32_t vectors_flash[16U + NUM_IRQS];
static uint32_t s_vectors[16U + NUM_IRQS] __attribute__((section(".ram_vectors"), aligned(1024)));

char g_out[OUT_SIZE] __attribute__((section(".noinit")));
uint32_t g_out_len;
volatile struct guard g_guard;
struct tether_fault g_last_fault;
struct tether_fault g_boot_fault;
struct status g_status;
uint32_t g_boot_csr;
uint32_t g_blink_until;
bool g_display_ok;
bool g_force_fs;

static uint32_t s_fault_count;

#define RETAINED ((volatile struct tether_fault *)(uintptr_t)TETHER_RETAINED)

void status_changed(void) {
  g_status.dirty = true;
}

/* Output buffer */

void out_write(const void *data, uint32_t len) {
  uint32_t room = OUT_SIZE - g_out_len;

  if (len > room) {
    len = room;
  }
  memcpy(&g_out[g_out_len], data, len);
  g_out_len += len;
}

int out_vprintf(const char *fmt, va_list ap) {
  uint32_t room = OUT_SIZE - g_out_len;

  if (room <= 1U) {
    return 0;
  }
  int n = fmt_vsnprintf(&g_out[g_out_len], room, fmt, ap);
  g_out_len += (uint32_t)n < room - 1U ? (uint32_t)n : room - 1U;
  return n;
}

int out_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = out_vprintf(fmt, ap);
  va_end(ap);
  return n;
}

/* Faults */

void __attribute__((noreturn)) system_reset(void) {
  usb_shutdown();
  __asm volatile("dsb" ::: "memory");
  wr(SCB_AIRCR, AIRCR_VECTKEY | AIRCR_SYSRESETREQ);
  __asm volatile("dsb" ::: "memory");
  for (;;) {
  }
}

static bool prv_ram(uint32_t a, uint32_t len) {
  return (a % 4U) == 0U && ((a >= 0x20000000U && a + len <= 0x20270000U) ||
                            (a >= 0x28000000U && a + len <= 0x28004000U));
}

static void prv_disable_irq(uint32_t irq) {
  wr(NVIC_ICER(irq / 32U), 1U << (irq % 32U));
  wr(NVIC_ICPR(irq / 32U), 1U << (irq % 32U));
}

/*
 * From fault_entry: records the fault and returns the code that unwinds the
 * guard, or 0 to carry on after an exception that only needed silencing.
 */
uint32_t fault_handle(const uint32_t *frame, const uint32_t *regs, uint32_t exc_return,
                      uint32_t ipsr) {
  const uint32_t exc = ipsr & 0x1ffU;
  struct tether_fault *f = &g_last_fault;

  if (exc >= 16U || exc == 11U || exc == 12U || exc == 14U) {
    /* An IRQ nobody handles, or SVC, DebugMon, PendSV: silenced, noted */
    if (exc >= 16U) {
      prv_disable_irq(exc - 16U);
    } else if (exc == 14U) {
      wr(SCB_ICSR, 1U << 27);
    }
    f->magic = TETHER_FAULT_MAGIC;
    f->exc = exc >= 16U ? 0x100U + (exc - 16U) : exc;
    f->uptime_ms = g_ms;
    f->count = ++s_fault_count;
    return 0U;
  }

  memset(f, 0, sizeof(*f));
  f->magic = TETHER_FAULT_MAGIC;
  f->exc = exc;
  f->exc_return = exc_return;
  f->cfsr = rd(SCB_CFSR);
  f->hfsr = rd(SCB_HFSR);
  f->mmfar = rd(SCB_MMFAR);
  f->bfar = rd(SCB_BFAR);
  f->afsr = rd(SCB_AFSR);
  if (prv_ram((uint32_t)(uintptr_t)frame, 32U)) {
    f->r0 = frame[0];
    f->r1 = frame[1];
    f->r2 = frame[2];
    f->r3 = frame[3];
    f->r12 = frame[4];
    f->lr = frame[5];
    f->pc = frame[6];
    f->xpsr = frame[7];
    f->sp = (uint32_t)(uintptr_t)frame + ((exc_return & 0x10U) ? 32U : 104U) +
            ((f->xpsr & (1U << 9)) ? 4U : 0U);
  } else {
    f->sp = (uint32_t)(uintptr_t)frame;
  }
  f->r4 = regs[0];
  f->r5 = regs[1];
  f->r6 = regs[2];
  f->r7 = regs[3];
  f->r8 = regs[4];
  f->r9 = regs[5];
  f->r10 = regs[6];
  f->r11 = regs[7];
  f->uptime_ms = g_ms;
  f->count = ++s_fault_count;

  wr(SCB_CFSR, f->cfsr);
  wr(SCB_HFSR, f->hfsr);
  if (exc == 2U) {
    /* NMI: a double ECC error of the flash, ECCD of FLASH_ECCR */
    wr(FLASHR(0x30U), 1U << 31);
  }
  status_changed();
  return TETHER_ERR_FAULT;
}

/* A fault the guard cannot unwind: kept for the next boot, then a reset */
void __attribute__((noreturn)) fault_fatal(uint32_t exc_return) {
  (void)exc_return;
  memcpy((void *)RETAINED, &g_last_fault, sizeof(g_last_fault));
  system_reset();
}

/* From SysTick_Handler: the time, and the end of a guarded call that overran */
uint32_t systick_tick(uint32_t exc_return) {
  g_ms++;
  if (g_guard.saved_msp == 0U) {
    return 0U;
  }
  watchdog_feed();
  if (g_guard.timeout_ms == 0U || g_ms - g_guard.start_ms <= g_guard.timeout_ms ||
      (exc_return & 8U) == 0U || (exc_return & 4U) == 0U) {
    return 0U;
  }
  struct tether_fault *f = &g_last_fault;
  uint32_t psp;
  __asm volatile("mrs %0, psp" : "=r"(psp));
  memset(f, 0, sizeof(*f));
  f->magic = TETHER_FAULT_MAGIC;
  f->exc = TETHER_FAULT_TIMEOUT;
  f->exc_return = exc_return;
  if (prv_ram(psp, 32U)) {
    const uint32_t *frame = (const uint32_t *)(uintptr_t)psp;
    f->r0 = frame[0];
    f->lr = frame[5];
    f->pc = frame[6];
    f->xpsr = frame[7];
  }
  f->sp = psp;
  f->uptime_ms = g_ms;
  f->count = ++s_fault_count;
  status_changed();
  return TETHER_ERR_TIMEOUT;
}

static void prv_systick_init(void) {
  wr(SYST_CSR, 0U);
  wr(SYST_RVR, (g_clock.sysclk_hz / 1000U) - 1U);
  wr(SYST_CVR, 0U);
  modb(SCB_SHPR3, 0xffU << 24, 0U);
  wr(SYST_CSR, 7U);
}

static void prv_vectors_init(void) {
  memcpy(s_vectors, vectors_flash, sizeof(s_vectors));
  __asm volatile("dsb" ::: "memory");
  wr(SCB_VTOR, (uint32_t)(uintptr_t)s_vectors);
  __asm volatile("dsb\n isb" ::: "memory");
  /* MemManage, BusFault, UsageFault and SecureFault on their own, for their status */
  setb(SCB_SHCSR, (1U << 16) | (1U << 17) | (1U << 18) | (1U << 19));
}

/* After a blob: its IRQs off, its handlers gone, the stub's tick back */
void irq_cleanup(void) {
  for (uint32_t i = 0U; i < IRQ_WORDS; i++) {
    wr(NVIC_ICER(i), 0xffffffffU);
    wr(NVIC_ICPR(i), 0xffffffffU);
  }
  prv_vectors_init();
  prv_systick_init();
  __asm volatile("msr basepri, %0\n cpsie i" ::"r"(0U) : "memory");
}

/* The API of blobs */

static int prv_api_i2c(int bus, uint8_t addr, const uint8_t *wrb, uint32_t wn, uint8_t *rdb,
                       uint32_t rn, int unsafe) {
  if (!unsafe && bus == 1 && wn > 1U && (addr == 0x61U || addr == 0x36U || addr == 0x29U)) {
    return TETHER_ERR_PROTECTED;
  }
  return i2c_xfer(bus, addr, wrb, wn, rdb, rn, false);
}

static void prv_api_display_update(void) {
  g_status.blob_owns_display = true;
  display_frame();
}

static void prv_api_display_release(void) {
  g_status.blob_owns_display = false;
  status_changed();
}

static void prv_api_set_irq(int irq, void (*handler)(void)) {
  if (irq >= 0 && (uint32_t)irq < NUM_IRQS) {
    s_vectors[16U + (uint32_t)irq] = (uint32_t)(uintptr_t)handler | 1U;
    __asm volatile("dsb" ::: "memory");
  }
}

static int prv_api_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = out_vprintf(fmt, ap);
  va_end(ap);
  return n;
}

static void prv_api_write(const void *data, uint32_t len) {
  out_write(data, len);
}

struct tether_api g_api = {
  .magic = TETHER_API_MAGIC,
  .version = TETHER_API_VERSION,
  .printf = prv_api_printf,
  .vprintf = out_vprintf,
  .write = prv_api_write,
  .delay_us = delay_us,
  .millis = millis,
  .cycles = cycles,
  .i2c = prv_api_i2c,
  .watchdog_feed = watchdog_feed,
  .fb = g_fb,
  .display_update = prv_api_display_update,
  .draw_text = display_text,
  .display_release = prv_api_display_release,
  .set_irq_handler = prv_api_set_irq,
  .out_size = OUT_SIZE,
};

/* Booting the PebbleOS slot */

void __attribute__((noreturn)) boot_image(uint32_t addr) {
  const uint32_t sp = rd(addr);
  const uint32_t pc = rd(addr + 4U);

  display_clear(TETHER_WHITE);
  display_text(120 - display_text_width("Booting", 2) / 2, 104, 2, TETHER_BLACK, TETHER_WHITE,
               "Booting");
  display_frame();

  usb_shutdown();
  /* The I2C the stub used, as the bootloader would leave them: reset */
  setb(RCC(RCC_APB1RSTR1_OFFSET), RCC_APB1ENR1_I2C1 | RCC_APB1ENR1_I2C2);
  setb(RCC(RCC_APB1RSTR2_OFFSET), RCC_APB1ENR2_I2C4);
  setb(RCC(RCC_APB3RSTR_OFFSET), RCC_APB3_I2C3);
  (void)rd(RCC(RCC_APB3RSTR_OFFSET));
  clrb(RCC(RCC_APB1RSTR1_OFFSET), RCC_APB1ENR1_I2C1 | RCC_APB1ENR1_I2C2);
  clrb(RCC(RCC_APB1RSTR2_OFFSET), RCC_APB1ENR2_I2C4);
  clrb(RCC(RCC_APB3RSTR_OFFSET), RCC_APB3_I2C3);

  __asm volatile("cpsid i" ::: "memory");
  wr(SYST_CSR, 0U);
  for (uint32_t i = 0U; i < IRQ_WORDS; i++) {
    wr(NVIC_ICER(i), 0xffffffffU);
    wr(NVIC_ICPR(i), 0xffffffffU);
  }
  wr(SCB_ICSR, (1U << 25) | (1U << 27));
  icache_invalidate();
  wr(SCB_VTOR, addr);
  __asm volatile("dsb\n isb" ::: "memory");
  boot_jump(sp, pc);
}

/* The screen */

static const char *prv_usb_text(void) {
  static char s_text[16];

  switch (g_usb.state) {
    case TETHER_USB_OFF:
      return "USB off";
    case TETHER_USB_WAITING:
      return "USB wait";
    case TETHER_USB_RESET:
      return g_usb.speed == 2U ? "USB HS..." : g_usb.speed == 1U ? "USB FS..." : "USB reset";
    case TETHER_USB_CONFIGURED:
      return g_usb.speed == 2U ? "USB HS ok" : "USB FS ok";
    case TETHER_USB_OPEN:
      return g_usb.speed == 2U ? "HS open" : "FS open";
    case TETHER_USB_SUSPENDED:
      return "USB idle";
    default:
      fmt_snprintf(s_text, sizeof(s_text), "USB err %u", g_usb.error);
      return s_text;
  }
}

static void prv_line(int y, int scale, uint8_t fg, uint8_t bg, const char *s) {
  display_text(120 - display_text_width(s, scale) / 2, y, scale, fg, bg, s);
}

static const char *prv_exc_name(uint32_t exc) {
  switch (exc) {
    case 2:
      return "NMI";
    case 3:
      return "HardFault";
    case 4:
      return "MemManage";
    case 5:
      return "BusFault";
    case 6:
      return "UsageFault";
    case 7:
      return "SecureFault";
    case TETHER_FAULT_TIMEOUT:
      return "Timeout";
    default:
      return exc >= 0x100U ? "IRQ" : "Fault";
  }
}

static void prv_draw(void) {
  char line[40];
  const uint8_t usb_color = g_usb.state == TETHER_USB_OPEN         ? TETHER_GREEN
                            : g_usb.state == TETHER_USB_CONFIGURED ? TETHER_GREEN
                            : g_usb.state == TETHER_USB_ERROR      ? TETHER_RED
                                                                   : TETHER_BLACK;

  display_clear(TETHER_WHITE);
  for (int y = 14; y < 46; y++) {
    memset(&g_fb[y * 240], TETHER_BLUE, 240);
  }
  prv_line(22, 2, TETHER_WHITE, TETHER_BLUE, "Tether v1");
  fmt_snprintf(line, 24, "%s", TETHER_VERSION);
  prv_line(52, 1, TETHER_BLACK, TETHER_WHITE, line);

  prv_line(68, 2, usb_color, TETHER_WHITE, prv_usb_text());

  fmt_snprintf(line, sizeof(line), "cmds %lu  err %lu", (unsigned long)g_status.cmds,
               (unsigned long)g_status.errors);
  prv_line(92, 1, TETHER_BLACK, TETHER_WHITE, line);
  prv_line(106, 1, TETHER_BLACK, TETHER_WHITE,
           g_status.last_cmd[0] != '\0' ? g_status.last_cmd : "no command yet");
  if (g_status.note[0] != '\0') {
    prv_line(120, 1, TETHER_BLUE, TETHER_WHITE, g_status.note);
  }

  const struct tether_fault *f = g_last_fault.magic == TETHER_FAULT_MAGIC ? &g_last_fault : NULL;
  if (f != NULL) {
    fmt_snprintf(line, sizeof(line), "%s #%lu", prv_exc_name(f->exc), (unsigned long)f->count);
    prv_line(136, 1, TETHER_RED, TETHER_WHITE, line);
    fmt_snprintf(line, sizeof(line), "pc %08lx", (unsigned long)f->pc);
    prv_line(148, 1, TETHER_RED, TETHER_WHITE, line);
    fmt_snprintf(line, sizeof(line), "cfsr %08lx", (unsigned long)f->cfsr);
    prv_line(160, 1, TETHER_RED, TETHER_WHITE, line);
  } else {
    prv_line(136, 1, TETHER_BLACK, TETHER_WHITE, "no fault");
  }
  if (g_boot_fault.magic == TETHER_FAULT_MAGIC) {
    fmt_snprintf(line, sizeof(line), "reset by %s", prv_exc_name(g_boot_fault.exc));
    prv_line(174, 1, TETHER_RED, TETHER_WHITE, line);
    fmt_snprintf(line, sizeof(line), "pc %08lx", (unsigned long)g_boot_fault.pc);
    prv_line(186, 1, TETHER_RED, TETHER_WHITE, line);
  }

  fmt_snprintf(line, sizeof(line), "%luMHz%s", (unsigned long)(g_clock.sysclk_hz / 1000000U),
               g_force_fs ? " FS" : "");
  prv_line(200, 1, TETHER_BLACK, TETHER_WHITE, line);
  prv_line(212, 1, TETHER_BLACK, TETHER_WHITE,
           port_boot_check(TETHER_SLOT_BASE) == TETHER_OK ? "Up: boot slot" : "slot empty");
}

static void prv_redraw(void) {
  prv_draw();
  display_frame();
  g_status.dirty = false;
}

/* Buttons, sampled every 10 ms */

static void prv_buttons(void) {
  static uint32_t s_next, s_held_since;
  static uint8_t s_last, s_stable, s_count;

  if ((int32_t)(millis() - s_next) < 0) {
    return;
  }
  s_next = millis() + 10U;
  uint8_t now = buttons_read();
  if (now != s_last) {
    s_last = now;
    s_count = 0U;
    return;
  }
  if (s_count < 3U) {
    s_count++;
    return;
  }
  const uint8_t pressed = now & (uint8_t)~s_stable;
  const uint8_t released = s_stable & (uint8_t)~now;
  s_stable = now;

  /* Back and Select held for 2 s: reset */
  if ((now & 0x5U) == 0x5U) {
    if (s_held_since == 0U) {
      s_held_since = millis() | 1U;
    } else if (millis() - s_held_since > 2000U) {
      display_clear(TETHER_WHITE);
      prv_line(104, 2, TETHER_BLACK, TETHER_WHITE, "Reset");
      display_frame();
      system_reset();
    }
  } else {
    s_held_since = 0U;
  }
  if (pressed != 0U && g_status.blob_owns_display) {
    g_status.blob_owns_display = false;
    status_changed();
  }
  /* Up, on its own: boot the slot */
  if ((released & 0x2U) && now == 0U && port_boot_check(TETHER_SLOT_BASE) == TETHER_OK) {
    boot_image(TETHER_SLOT_BASE);
  }
}

static void prv_quiesce(void) {
  /* Whatever the bootloader left enabled, pending or mastering the bus */
  wr(SYST_CSR, 0U);
  for (uint32_t i = 0U; i < IRQ_WORDS; i++) {
    wr(NVIC_ICER(i), 0xffffffffU);
    wr(NVIC_ICPR(i), 0xffffffffU);
  }
  wr(SCB_ICSR, (1U << 25) | (1U << 27));
  wr(EXTI_BASE + 0x80U, 0U);          /* IMR1 */
  wr(EXTI_BASE + 0x84U, 0U);          /* EMR1 */
  wr(EXTI_BASE + 0x0cU, 0xffffffffU); /* RPR1 */
  wr(EXTI_BASE + 0x10U, 0xffffffffU); /* FPR1 */
  /* GPDMA1 and DMA2D, SDMMC1, LPDMA1 */
  const uint32_t ahb1 = (1U << 0) | (1U << 18);
  setb(RCC(STM32U5_RCC_AHB1RSTR_OFFSET), ahb1);
  (void)rd(RCC(STM32U5_RCC_AHB1RSTR_OFFSET));
  clrb(RCC(STM32U5_RCC_AHB1RSTR_OFFSET), ahb1);
  setb(RCC(RCC_AHB2RSTR1_OFFSET), RCC_AHB2_SDMMC1);
  (void)rd(RCC(RCC_AHB2RSTR1_OFFSET));
  clrb(RCC(RCC_AHB2RSTR1_OFFSET), RCC_AHB2_SDMMC1);
  setb(RCC(RCC_AHB3RSTR_OFFSET), 1U << 9);
  (void)rd(RCC(RCC_AHB3RSTR_OFFSET));
  clrb(RCC(RCC_AHB3RSTR_OFFSET), 1U << 9);
}

static void prv_icache_on(void) {
  uintptr_t cr = ICACHE_BASE + STM32U5_ICACHE_CR_OFFSET;

  icache_invalidate();
  while ((rd(ICACHE_BASE + STM32U5_ICACHE_SR_OFFSET) & ICACHE_SR_BUSYF) != 0U) {
  }
  setb(cr, ICACHE_CR_EN);
}

int main(void) {
  /* Why we are here, before the flags go */
  g_boot_csr = rd(RCC(STM32U5_RCC_CSR_OFFSET));
  setb(RCC(STM32U5_RCC_CSR_OFFSET), RCC_CSR_RMVF);
  if (RETAINED->magic == TETHER_FAULT_MAGIC) {
    memcpy(&g_boot_fault, (const void *)RETAINED, sizeof(g_boot_fault));
    RETAINED->magic = 0U;
  }
  watchdog_feed();

  setb(RCC(STM32U5_RCC_AHB3ENR_OFFSET), RCC_AHB3ENR_PWREN);
  /* SRAM1, SRAM2, SRAM3 and SRAM5 clocked, as they are after a reset: blobs use them all */
  setb(RCC(STM32U5_RCC_AHB1ENR_OFFSET), 1U << 31);
  setb(RCC(STM32U5_RCC_AHB2ENR1_OFFSET), 3U << 30);
  setb(RCC(STM32U5_RCC_AHB2ENR1_OFFSET + 4U), 1U << 31);
  prv_quiesce();
  prv_vectors_init();
  clock_init();
  prv_icache_on();
  __asm volatile("cpsie i" ::: "memory");

  gpio_clocks_on();
  gpio_pins_init();
  /* Down held at boot: full speed only */
  delay_ms(5U);
  g_force_fs = (buttons_read() & 0x8U) != 0U;

  g_api.cpu_hz = g_clock.sysclk_hz;
  g_api.out_buf = (uint32_t)(uintptr_t)g_out;

  g_display_ok = display_init();
  prv_redraw();

  usb_init(g_force_fs);
  uint32_t last_draw = millis();
  uint32_t last_rx = 0U;

  for (;;) {
    uint8_t buf[512];

    watchdog_feed();
    usb_poll();
    size_t n = usb_read(buf, sizeof(buf));
    if (n != 0U) {
      proto_input(buf, n);
      last_rx = millis();
    }
    proto_poll();
    prv_buttons();

    /* The frontlight blinks off with each command */
    gpio_write(PORT_F, 3, (int32_t)(millis() - g_blink_until) < 0);

    /* A frame takes 35 ms: not while the host enumerates or a command is coming in */
    if (g_status.dirty && !g_status.blob_owns_display && millis() - last_draw > 250U &&
        usb_quiet(300U) && millis() - last_rx > 100U) {
      prv_redraw();
      last_draw = millis();
    }
  }
}
