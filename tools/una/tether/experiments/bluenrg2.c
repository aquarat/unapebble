/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Talks to the BlueNRG-2 on the link1 module over SPI1's pins, bit-banged in
 * mode 1 (CPOL 0, CPHA 1) at about 250 kHz: resets it, prints the event it
 * starts with, and sends HCI and ACI commands, printing every packet.
 *
 *   args[0] = 0  versions and read-only queries (the default)
 *   args[0] = 1  the same, after switching it to link-layer-only mode
 *                (aci_hal_write_config_data 0x2c, held in its RAM until reset)
 *   args[0] = 2  standard HCI commands only, as a host would send them
 *   args[0] = 3  a reset with MISO (the chip's IO3, ST's updater pin) pulled
 *                up, then one with it floating: the start events
 *   args[0] = 4  three resets with MISO pulled down, hci_reset, another reset
 *   args[0] = 5  a 3 s reset then one of 5, 20, 100, 300, 1000, 3000 ms, in turn
 *   args[0] = 6  a normal start, a 5 ms reset (which starts the updater), then
 *                RESET held low 2 s with the pins in args[1] put back as found
 *                (bit 0 SCK, 1 MISO, 2 MOSI, 3 CS, 4 IRQ), the SPI pins set
 *                up again and a 5 ms reset (which starts normally)
 *   args[0] = 7  a normal start (2 s in reset, SCK, MOSI and CS analog), link-layer-only mode
 *                written and read back, then a restart from args[1] ms in reset (5 by default)
 *                with those lines analog again, CS high from 5 ms before the release: its
 *                start reason, and the mode read back
 *   args[0] = 8  a normal start as in mode 7, then hci_reset (approved for this test, decision
 *                261127-233626-01) with SCK, MOSI and CS made analog as soon as its Command
 *                Complete is read; the restart's IRQ and start reason; on a normal start,
 *                link-layer-only mode written and read back; otherwise the chip is recovered
 *                from 2 s in reset with the lines analog. args[1]: bit 0 keeps CS driven high
 *                through the restart, bit 1 writes link-layer-only mode before hci_reset too
 *   args[0] = 9  read only: a normal start, aci_hal_updater_start (0xfc20) with the lines released
 *                as in mode 8, the updater's version, buffer size and hardware version, the
 *                flash from 0x10042000 to 0x10080000 read in 64-byte blocks (0xfc28) into RAM at
 *                args[1] (0x20100000 by default), the updater's CRC of it and of the page at
 *                0x1007f000 (0xfc29), then 0xfc21 to restart, and the start reason checked.
 *                Nothing is erased or written. Recovered from 2 s in reset if the restart is
 *                not a normal one.
 *   args[0] = 10 checks before a write, read only: a read below the DTM (0x10040000) through the
 *                updater, which must be refused, then a restart with the SPI lines driven,
 *                which must start the updater (reason 4), as it would with a broken DTM, and
 *                0xfc21 back to a normal start
 *   args[0] = 11 writes the BlueNRG-2's flash through its updater, from a table in RAM at args[1]
 *                (bluenrg2/patch.py): magic, count, then per page its address, the RAM address
 *                of its 2048 bytes and flags (1 erase first, 2 program). Only whole 2 KiB pages
 *                from 0x10042800 to 0x1007efff: never the DTM's first page, with its vector
 *                table and BLUE flag, nor the stack's data at 0x1007f000. Each page is read back
 *                and checked against the updater's CRC; the first failure stops the run, and
 *                the chip stays in its updater. Only when all pass, 0xfc21 and a normal start.
 *   args[0] = 13 as mode 11, the way ST's updater tools do it: the BLUE flag erased (0xfc24) first,
 *                the DTM's first page then allowed, given with its flag word blank, the pages
 *                written and checked, the flag set (0xfc25) and read back, then the restart
 *   args[0] = 12 a probe of a chip that does not start: RESET held low args[1] ms (10000 by
 *                default) with every SPI line and IRQ analog, released, IRQ sampled every
 *                100 ms for 5 s, then a version query written blindly, an IRQ or not
 *   args[1] != 0 in modes 0 to 2, also advertise for args[1] seconds (at most
 *                10) with standard HCI commands, non-connectable, as
 *                "unapebble-test"
 *
 * hci_reset, and a reset soon after a start, leave the chip in its updater
 * (start reason 4), where it answers no command: see hardware.md 2.5.1.
 * Nothing is written to the BlueNRG-2's flash: no updater command, no
 * aci_hal_write_config_data but offset 0x2c (RAM). The pins are put back as
 * they were at the end, and the chip is left in reset if it was in reset.
 */

#include "tether_blob.h"

#define RCC_AHB2ENR1 (0x46020c00U + 0x8cU)

#define PA 0U
#define PB 1U
#define PG 6U

#define SCK_PORT  PG
#define SCK_PIN   2U
#define MISO_PORT PG
#define MISO_PIN  3U
#define MOSI_PORT PB
#define MOSI_PIN  5U
#define CS_PORT   PA
#define CS_PIN    15U
#define IRQ_PORT  PA
#define IRQ_PIN   9U
#define RST_PORT  PA
#define RST_PIN   8U

#define HALF_US 2U

struct pin_save {
  uint32_t port, pin, moder, otyper, pupdr, odr;
};

static struct pin_save s_save[6];
static uint8_t s_pkt[300];
static int s_errors;
/* No per-packet output, for the many reads of mode 9 */
static bool s_quiet;

static void prv_save(struct pin_save *s, uint32_t port, uint32_t pin) {
  const uint32_t b = GPIO_PORT(port);
  s->port = port;
  s->pin = pin;
  s->moder = (REG32(b + 0x00U) >> (pin * 2U)) & 3U;
  s->otyper = (REG32(b + 0x04U) >> pin) & 1U;
  s->pupdr = (REG32(b + 0x0cU) >> (pin * 2U)) & 3U;
  s->odr = (REG32(b + 0x14U) >> pin) & 1U;
}

static void prv_restore(const struct pin_save *s) {
  const uint32_t b = GPIO_PORT(s->port);
  REG32(b + 0x18U) = s->odr ? (1U << s->pin) : (1U << (s->pin + 16U));
  REG32(b + 0x04U) = (REG32(b + 0x04U) & ~(1U << s->pin)) | (s->otyper << s->pin);
  REG32(b + 0x0cU) = (REG32(b + 0x0cU) & ~(3U << (s->pin * 2U))) | (s->pupdr << (s->pin * 2U));
  REG32(b + 0x00U) = (REG32(b + 0x00U) & ~(3U << (s->pin * 2U))) | (s->moder << (s->pin * 2U));
}

static void prv_out(uint32_t port, uint32_t pin, bool od, uint32_t level) {
  const uint32_t b = GPIO_PORT(port);
  REG32(b + 0x18U) = level ? (1U << pin) : (1U << (pin + 16U));
  REG32(b + 0x04U) = (REG32(b + 0x04U) & ~(1U << pin)) | ((od ? 1U : 0U) << pin);
  REG32(b + 0x0cU) &= ~(3U << (pin * 2U));
  REG32(b + 0x08U) |= 2U << (pin * 2U);
  REG32(b + 0x00U) = (REG32(b + 0x00U) & ~(3U << (pin * 2U))) | (1U << (pin * 2U));
}

static void prv_in(uint32_t port, uint32_t pin) {
  const uint32_t b = GPIO_PORT(port);
  REG32(b + 0x0cU) &= ~(3U << (pin * 2U));
  REG32(b + 0x00U) &= ~(3U << (pin * 2U));
}

static void prv_analog(uint32_t port, uint32_t pin) {
  const uint32_t b = GPIO_PORT(port);
  REG32(b + 0x0cU) &= ~(3U << (pin * 2U));
  REG32(b + 0x00U) |= 3U << (pin * 2U);
}

static inline void prv_set(uint32_t port, uint32_t pin, uint32_t level) {
  REG32(GPIO_PORT(port) + 0x18U) = level ? (1U << pin) : (1U << (pin + 16U));
}

static inline uint32_t prv_get(uint32_t port, uint32_t pin) {
  return (REG32(GPIO_PORT(port) + 0x10U) >> pin) & 1U;
}

static inline bool prv_irq(void) {
  return prv_get(IRQ_PORT, IRQ_PIN) != 0U;
}

/* Mode 1: both sides shift on the rising edge and sample on the falling one */
static uint8_t prv_xfer(uint8_t out) {
  uint8_t in = 0U;
  for (int i = 7; i >= 0; i--) {
    prv_set(SCK_PORT, SCK_PIN, 1U);
    prv_set(MOSI_PORT, MOSI_PIN, (out >> i) & 1U);
    tether->delay_us(HALF_US);
    in = (uint8_t)((in << 1) | prv_get(MISO_PORT, MISO_PIN));
    prv_set(SCK_PORT, SCK_PIN, 0U);
    tether->delay_us(HALF_US);
  }
  return in;
}

static bool prv_wait_irq(bool level, uint32_t ms) {
  const uint32_t t0 = tether->millis();
  while (prv_irq() != level) {
    if (tether->millis() - t0 > ms) {
      return false;
    }
  }
  return true;
}

static void prv_dump(const char *what, const uint8_t *p, uint32_t n) {
  tprintf("%s %lu:", what, (unsigned long)n);
  for (uint32_t i = 0U; i < n; i++) {
    tprintf(" %02x", p[i]);
  }
  tprintf("\n");
}

/* One read transaction, while IRQ is high; returns the byte count */
static int prv_read(uint8_t *buf, uint32_t size, bool verbose) {
  uint8_t h[5];
  prv_set(CS_PORT, CS_PIN, 0U);
  tether->delay_us(2U);
  h[0] = prv_xfer(0x0bU);
  for (int i = 1; i < 5; i++) {
    h[i] = prv_xfer(0x00U);
  }
  uint32_t n = (uint32_t)h[3] | ((uint32_t)h[4] << 8);
  if (n == 0xffffU) {
    n = 0U;
  }
  if (n > size) {
    n = size;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = prv_xfer(0xffU);
  }
  if (!prv_wait_irq(false, 10U)) {
    tprintf("read: IRQ stays high\n");
  }
  prv_set(CS_PORT, CS_PIN, 1U);
  tether->delay_us(100U);
  if (verbose) {
    tprintf("rd hdr %02x %02x %02x %02x %02x\n", h[0], h[1], h[2], h[3], h[4]);
  }
  return (int)n;
}

/* One packet out; retries while the chip has no room */
static int prv_write(const uint8_t *buf, uint32_t n) {
  for (int attempt = 0; attempt < 50; attempt++) {
    /* A pending event must be read first */
    if (prv_irq()) {
      int r = prv_read(s_pkt, sizeof(s_pkt), false);
      prv_dump("evt (before write)", s_pkt, (uint32_t)r);
      continue;
    }
    uint8_t h[5];
    prv_set(CS_PORT, CS_PIN, 0U);
    if (!prv_wait_irq(true, 10U)) {
      prv_set(CS_PORT, CS_PIN, 1U);
      tether_delay_ms(2U);
      continue;
    }
    h[0] = prv_xfer(0x0aU);
    for (int i = 1; i < 5; i++) {
      h[i] = prv_xfer(0x00U);
    }
    const uint32_t wbuf = (uint32_t)h[1] | ((uint32_t)h[2] << 8);
    const bool ok = wbuf != 0xffffU && wbuf >= n;
    if (ok) {
      for (uint32_t i = 0U; i < n; i++) {
        (void)prv_xfer(buf[i]);
      }
    }
    (void)prv_wait_irq(false, 10U);
    prv_set(CS_PORT, CS_PIN, 1U);
    tether_delay_ms(2U);
    if (ok) {
      if (attempt != 0) {
        tprintf("write: %d retries\n", attempt);
      }
      return 0;
    }
    tprintf("write: hdr %02x %02x %02x %02x %02x, need %lu\n", h[0], h[1], h[2], h[3], h[4],
            (unsigned long)n);
  }
  s_errors++;
  tprintf("write: gave up\n");
  return -1;
}

/* Sends a command, prints the packets until its Command Complete or Status;
   returns the status byte, or -1. The parameters of the reply go to ret. */
static int prv_cmd(const char *name, uint16_t op, const uint8_t *par, uint8_t len, uint8_t *ret,
                   uint32_t *ret_len) {
  uint8_t c[4 + 255];
  c[0] = 0x01U;
  c[1] = (uint8_t)op;
  c[2] = (uint8_t)(op >> 8);
  c[3] = len;
  for (uint32_t i = 0U; i < len; i++) {
    c[4 + i] = par[i];
  }
  if (!s_quiet) {
    tprintf("> %s (%04x)\n", name, op);
  }
  if (prv_write(c, 4U + len) != 0) {
    return -1;
  }
  const uint32_t t0 = tether->millis();
  while (tether->millis() - t0 < 1000U) {
    tether->watchdog_feed();
    if (!prv_irq()) {
      continue;
    }
    const int n = prv_read(s_pkt, sizeof(s_pkt), false);
    if (!s_quiet) {
      prv_dump("<", s_pkt, (uint32_t)n);
    }
    if (n >= 7 && s_pkt[0] == 0x04U && s_pkt[1] == 0x0eU &&
        (s_pkt[4] | (s_pkt[5] << 8)) == op) {
      if (ret != NULL) {
        *ret_len = (uint32_t)n - 6U;
        for (int i = 6; i < n; i++) {
          ret[i - 6] = s_pkt[i];
        }
      }
      return s_pkt[6];
    }
    if (n >= 7 && s_pkt[0] == 0x04U && s_pkt[1] == 0x0fU &&
        (s_pkt[5] | (s_pkt[6] << 8)) == op) {
      return s_pkt[3];
    }
  }
  s_errors++;
  tprintf("  no reply\n");
  return -1;
}

static uint32_t s_reset_ms = 5U;

static void prv_reset(void) {
  prv_set(CS_PORT, CS_PIN, 1U);
  prv_set(RST_PORT, RST_PIN, 0U);
  tether_delay_ms(s_reset_ms);
  prv_set(RST_PORT, RST_PIN, 1U);
  const uint32_t t0 = tether->millis();
  tether->delay_us(50U);
  tprintf("reset released: RESET reads %lu, IRQ %lu, MISO %lu\n",
          (unsigned long)prv_get(RST_PORT, RST_PIN), (unsigned long)prv_irq(),
          (unsigned long)prv_get(MISO_PORT, MISO_PIN));
  bool up = false;
  while (!up && tether->millis() - t0 < 3000U) {
    tether->watchdog_feed();
    up = prv_irq();
  }
  tprintf("reset: IRQ %s after %lu ms\n", up ? "high" : "never high",
          (unsigned long)(tether->millis() - t0));
  if (up) {
    const int n = prv_read(s_pkt, sizeof(s_pkt), true);
    prv_dump("start event", s_pkt, (uint32_t)n);
    if (n >= 6 && s_pkt[1] == 0xffU && s_pkt[3] == 0x01U && s_pkt[4] == 0x00U) {
      tprintf("  aci_blue_initialized_event, reason %u\n", s_pkt[5]);
    }
  } else {
    s_errors++;
  }
}

/* The start event after a reset; CS stays high meanwhile, as CS low while the
   chip starts can send it into its updater */
static void prv_wait_start(void) {
  const uint32_t t0 = tether->millis();
  bool up = false;
  while (!up && tether->millis() - t0 < 3000U) {
    tether->watchdog_feed();
    up = prv_irq();
  }
  if (!up) {
    s_errors++;
    tprintf("  no start event\n");
    return;
  }
  const int n = prv_read(s_pkt, sizeof(s_pkt), false);
  tprintf("  after %lu ms\n", (unsigned long)(tether->millis() - t0));
  prv_dump("<", s_pkt, (uint32_t)n);
}

static void prv_simple(const char *name, uint16_t op) {
  uint8_t r[260];
  uint32_t n = 0U;
  const int st = prv_cmd(name, op, NULL, 0U, r, &n);
  tprintf("  status %d\n", st);
  if (op == 0x0c03U && st == 0) {
    prv_wait_start();
  }
}

static void prv_versions(void) {
  uint8_t r[260];
  uint32_t n = 0U;
  if (prv_cmd("hci_read_local_version_information", 0x1001U, NULL, 0U, r, &n) == 0 && n >= 9U) {
    tprintf("  HCI version %u, revision %04x, LMP version %u, manufacturer %04x, subversion %04x\n",
            r[1], r[2] | (r[3] << 8), r[4], r[5] | (r[6] << 8), r[7] | (r[8] << 8));
  }
  if (prv_cmd("hci_read_bd_addr", 0x1009U, NULL, 0U, r, &n) == 0 && n >= 7U) {
    tprintf("  BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x\n", r[6], r[5], r[4], r[3], r[2], r[1]);
  }
}

static void prv_aci_versions(void) {
  uint8_t r[260];
  uint32_t n = 0U;
  if (prv_cmd("aci_hal_get_fw_build_number", 0xfc00U, NULL, 0U, r, &n) == 0 && n >= 3U) {
    tprintf("  build number %u\n", r[1] | (r[2] << 8));
  }
  if (prv_cmd("aci_hal_get_firmware_details", 0xfc01U, NULL, 0U, r, &n) == 0 && n >= 15U) {
    tprintf("  DTM v%u.%u.%u variant %u (1 UART, 2 SPI) build %u\n", r[1], r[2], r[3], r[4],
            r[5] | (r[6] << 8));
    tprintf("  stack v%u.%u.%u development %u variant %04x build %u\n", r[7], r[8], r[9], r[10],
            r[11] | (r[12] << 8), r[13] | (r[14] << 8));
  }
  static const struct {
    uint8_t off;
    const char *name;
  } s_cfg[] = {{0x00U, "public address"}, {0x80U, "static random address"},
               {0x2cU, "link layer only"}};
  for (unsigned i = 0U; i < sizeof(s_cfg) / sizeof(s_cfg[0]); i++) {
    tprintf("  config data %02x, %s:\n", s_cfg[i].off, s_cfg[i].name);
    (void)prv_cmd("aci_hal_read_config_data", 0xfc0dU, &s_cfg[i].off, 1U, r, &n);
  }
}

static void prv_hci_queries(void) {
  static const struct {
    uint16_t op;
    const char *name;
  } s_q[] = {
    {0x1002U, "hci_read_local_supported_commands"},
    {0x1003U, "hci_read_local_supported_features"},
    {0x1005U, "hci_read_buffer_size"},
    {0x2002U, "hci_le_read_buffer_size"},
    {0x2003U, "hci_le_read_local_supported_features"},
    {0x201cU, "hci_le_read_supported_states"},
    {0x200fU, "hci_le_read_white_list_size"},
    {0x202aU, "hci_le_read_resolving_list_size"},
    {0x202fU, "hci_le_read_maximum_data_length"},
    {0x2007U, "hci_le_read_advertising_channel_tx_power"},
  };
  static const uint8_t s_mask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};
  static const uint8_t s_le_mask[8] = {0xff, 0x0f, 0, 0, 0, 0, 0, 0};
  uint8_t r[260];
  uint32_t n;
  for (unsigned i = 0U; i < sizeof(s_q) / sizeof(s_q[0]); i++) {
    prv_simple(s_q[i].name, s_q[i].op);
  }
  /* What a host sends at start: event masks, which change nothing on air */
  tprintf("  status %d\n", prv_cmd("hci_set_event_mask", 0x0c01U, s_mask, 8U, r, &n));
  tprintf("  status %d\n", prv_cmd("hci_le_set_event_mask", 0x2001U, s_le_mask, 8U, r, &n));
}

static void prv_advertise(uint32_t seconds) {
  uint8_t r[260];
  uint32_t n;
  /* 100 ms, ADV_NONCONN_IND, public address, all channels */
  static const uint8_t s_par[15] = {0xa0, 0x00, 0xa0, 0x00, 0x03, 0x00, 0x00, 0,
                                    0,    0,    0,    0,    0,    0x07, 0x00};
  static const uint8_t s_data[32] = {
    17U,  0x02, 0x01, 0x04, 0x0f, 0x09, 'u', 'n', 'a', 'p', 'e', 'b',
    'b',  'l',  'e',  '-',  't',  'e',  's', 't', 0,   0,   0,   0,
    0,    0,    0,    0,    0,    0,    0,   0};
  static const uint8_t s_on = 1U;
  static const uint8_t s_off = 0U;
  tprintf("advertising for %lu s\n", (unsigned long)seconds);
  tprintf("  status %d\n",
          prv_cmd("hci_le_set_advertising_parameters", 0x2006U, s_par, 15U, r, &n));
  tprintf("  status %d\n", prv_cmd("hci_le_set_advertising_data", 0x2008U, s_data, 32U, r, &n));
  const int st = prv_cmd("hci_le_set_advertise_enable", 0x200aU, &s_on, 1U, r, &n);
  tprintf("  status %d\n", st);
  if (st == 0) {
    const uint32_t t0 = tether->millis();
    while (tether->millis() - t0 < seconds * 1000U) {
      tether->watchdog_feed();
      if (prv_irq()) {
        const int k = prv_read(s_pkt, sizeof(s_pkt), false);
        prv_dump("< (advertising)", s_pkt, (uint32_t)k);
      }
    }
  }
  tprintf("  status %d\n", prv_cmd("hci_le_set_advertise_enable", 0x200aU, &s_off, 1U, r, &n));
}


static void prv_lines_released(bool keep_cs) {
  prv_analog(SCK_PORT, SCK_PIN);
  prv_analog(MOSI_PORT, MOSI_PIN);
  if (!keep_cs) {
    prv_analog(CS_PORT, CS_PIN);
  }
}

static void prv_lines_driven(void) {
  prv_out(CS_PORT, CS_PIN, false, 1U);
  prv_out(SCK_PORT, SCK_PIN, false, 0U);
  prv_out(MOSI_PORT, MOSI_PIN, false, 0U);
}

/* A restart the chip makes itself after a command's reply, the lines released meanwhile: its
   start reason, or -1 */
static int prv_self_restart(void) {
  prv_lines_released(false);
  const uint32_t t0 = tether->millis();
  bool up = false;
  while (!up && tether->millis() - t0 < 3000U) {
    tether->watchdog_feed();
    up = prv_irq();
  }
  prv_lines_driven();
  int reason = -1;
  if (up) {
    const int k = prv_read(s_pkt, sizeof(s_pkt), false);
    if (k >= 6 && s_pkt[1] == 0xffU && s_pkt[3] == 0x01U && s_pkt[4] == 0x00U) {
      reason = s_pkt[5];
    }
  }
  tprintf("  restart: IRQ %s after %lu ms, reason %d\n", up ? "high" : "never high",
          (unsigned long)(tether->millis() - t0), reason);
  return reason;
}

/* 2 s in reset with the lines released, then a start: its reason */
static int prv_long_restart(void) {
  s_pkt[5] = 0U;
  prv_set(RST_PORT, RST_PIN, 0U);
  prv_lines_released(false);
  tether_delay_ms(2000U);
  prv_lines_driven();
  prv_reset();
  return s_pkt[5];
}

static uint32_t prv_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void prv_put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

#define FLASH_DTM   0x10042000U
#define FLASH_END   0x10080000U
#define FLASH_NVM   0x1007f000U
#define FLASH_PAGE  0x800U
#define READ_BLOCK  64U

static int prv_enter_updater(void);

static void prv_backup(uint32_t dest) {
  uint8_t r[260];
  uint32_t n;
  int st;

  if (prv_enter_updater() != 0) {
    s_errors++;
    tprintf("  no updater; recovery reason %d\n", prv_long_restart());
    return;
  }
  if (prv_cmd("updater_get_version", 0xfc22U, NULL, 0U, r, &n) == 0 && n >= 2U) {
    tprintf("  updater version %u\n", r[1]);
  }
  if (prv_cmd("updater_get_bufsize", 0xfc23U, NULL, 0U, r, &n) == 0 && n >= 3U) {
    tprintf("  buffer size %u\n", r[1] | (r[2] << 8));
  }
  if (prv_cmd("updater_read_hw_version", 0xfc2aU, NULL, 0U, r, &n) == 0 && n >= 2U) {
    tprintf("  hardware version %u\n", r[1]);
  }

  s_quiet = true;
  uint32_t bad = 0U;
  for (uint32_t a = FLASH_DTM; a < FLASH_END; a += READ_BLOCK) {
    uint8_t par[6];
    prv_put_le32(par, a);
    par[4] = (uint8_t)READ_BLOCK;
    par[5] = 0U;
    st = prv_cmd("updater_read_data_block", 0xfc28U, par, 6U, r, &n);
    if (st != 0 || n != 1U + READ_BLOCK) {
      bad++;
      if (bad <= 4U) {
        tprintf("  read %08lx: status %d, %lu bytes\n", (unsigned long)a, st, (unsigned long)n);
      }
      for (uint32_t i = 0U; i < READ_BLOCK; i++) {
        ((volatile uint8_t *)dest)[a - FLASH_DTM + i] = 0U;
      }
      continue;
    }
    for (uint32_t i = 0U; i < READ_BLOCK; i++) {
      ((volatile uint8_t *)dest)[a - FLASH_DTM + i] = r[1 + i];
    }
    if (((a - FLASH_DTM) & 0x7fffU) == 0U) {
      tether->watchdog_feed();
    }
  }
  s_quiet = false;
  tprintf("  read %lu bytes to %08lx, %lu blocks failed\n", (unsigned long)(FLASH_END - FLASH_DTM),
          (unsigned long)dest, (unsigned long)bad);
  if (bad != 0U) {
    s_errors++;
  }

  /* The updater's CRC, 8 pages a command: more takes it past the reply timeout */
  for (uint32_t a = FLASH_DTM; a < FLASH_END; a += 8U * FLASH_PAGE) {
    uint8_t par[5];
    const uint32_t pages = (FLASH_END - a) / FLASH_PAGE < 8U ? (FLASH_END - a) / FLASH_PAGE : 8U;
    prv_put_le32(par, a);
    par[4] = (uint8_t)pages;
    st = prv_cmd("updater_calc_crc", 0xfc29U, par, 5U, r, &n);
    if (st == 0 && n >= 5U) {
      tprintf("  crc %08lx %lu %08lx\n", (unsigned long)a, (unsigned long)pages,
              (unsigned long)prv_le32(&r[1]));
    } else {
      s_errors++;
      tprintf("  crc %08lx: status %d\n", (unsigned long)a, st);
    }
  }

  st = prv_cmd("updater_reboot", 0xfc21U, NULL, 0U, r, &n);
  tprintf("  status %d\n", st);
  if (prv_self_restart() != 1) {
    s_errors++;
    tprintf("  recovery reason %d\n", prv_long_restart());
  }
  prv_aci_versions();
}

static uint32_t prv_crc(const volatile uint8_t *p, uint32_t len) {
  uint32_t crc = 0U;
  for (uint32_t i = 0U; i < len; i += 4U) {
    crc ^= (uint32_t)p[i] | ((uint32_t)p[i + 1U] << 8) | ((uint32_t)p[i + 2U] << 16) |
           ((uint32_t)p[i + 3U] << 24);
    for (int j = 0; j < 32; j++) {
      crc = (crc << 1) ^ ((crc >> 31) != 0U ? 0x04c11db7U : 0U);
    }
  }
  return crc;
}

/* A start with IO3 (MISO) held high through the release of RESET, the updater's boot pin in ST's
   DTM: pulled up (pull 1) or driven (pull 2) for hold_ms after the release, then an input again.
   Its start reason, or -1. */
static int prv_start_io3_high(uint32_t pull, uint32_t hold_ms) {
  const uint32_t b = GPIO_PORT(MISO_PORT);

  prv_set(RST_PORT, RST_PIN, 0U);
  prv_lines_released(false);
  if (pull == 1U) {
    REG32(b + 0x0cU) = (REG32(b + 0x0cU) & ~(3U << (MISO_PIN * 2U))) | (1U << (MISO_PIN * 2U));
  } else if (pull == 2U) {
    prv_out(MISO_PORT, MISO_PIN, false, 1U);
  }
  tether_delay_ms(100U);
  prv_out(CS_PORT, CS_PIN, false, 1U);
  prv_out(SCK_PORT, SCK_PIN, false, 0U);
  /* ST's updater source: "IO3 -> SPI MOSI PIN" */
  prv_out(MOSI_PORT, MOSI_PIN, false, pull == 3U ? 1U : 0U);
  tether_delay_ms(5U);
  prv_set(RST_PORT, RST_PIN, 1U);
  const uint32_t t0 = tether->millis();
  bool up = false;
  /* hold_ms 0: held until IRQ rises, through any restarts of the chip meanwhile */
  if (hold_ms != 0U) {
    tether_delay_ms(hold_ms);
    prv_in(MISO_PORT, MISO_PIN);
    prv_set(MOSI_PORT, MOSI_PIN, 0U);
  }
  while (!up && tether->millis() - t0 < 4000U) {
    tether->watchdog_feed();
    up = prv_irq();
  }
  prv_in(MISO_PORT, MISO_PIN);
  prv_set(MOSI_PORT, MOSI_PIN, 0U);
  int reason = -1;
  if (up) {
    const int k = prv_read(s_pkt, sizeof(s_pkt), false);
    prv_dump("  start event", s_pkt, (uint32_t)k);
    if (k >= 6 && s_pkt[1] == 0xffU && s_pkt[3] == 0x01U && s_pkt[4] == 0x00U) {
      reason = s_pkt[5];
    }
  }
  tprintf("  %s for %lu ms: IRQ %s after %lu ms, reason %d\n",
          pull == 1U ? "MISO pulled up" : pull == 2U ? "MISO driven high" : "MOSI driven high", (unsigned long)hold_ms,
          up ? "high" : "never high", (unsigned long)(tether->millis() - t0), reason);
  return reason;
}

/* The updater by its boot pin, whatever the DTM does: IO3 pulled up, then driven */
static int prv_updater_by_pin(void) {
  tprintf("updater by the boot pin:\n");
  return prv_start_io3_high(3U, 0U) == 4 ? 0 : -1;
}

static int prv_enter_updater(void) {
  uint8_t r[260];
  uint32_t n;

  tprintf("normal start:\n");
  if (prv_long_restart() != 1) {
    s_errors--;
    tprintf("  no normal start\n");
    return prv_updater_by_pin();
  }
  if (prv_cmd("aci_hal_updater_start", 0xfc20U, NULL, 0U, r, &n) != 0) {
    return -1;
  }
  return prv_self_restart() == 2 ? 0 : -1;
}

static void prv_checks(void) {
  uint8_t r[260];
  uint32_t n;
  uint8_t par[6];

  if (prv_enter_updater() != 0) {
    s_errors++;
    tprintf("  no updater; recovery reason %d\n", prv_long_restart());
    return;
  }
  prv_put_le32(par, 0x10040000U);
  par[4] = 16U;
  par[5] = 0U;
  const int st = prv_cmd("updater_read_data_block 0x10040000", 0xfc28U, par, 6U, r, &n);
  tprintf("  read below the DTM: status %d, %lu bytes: %s\n", st, (unsigned long)n,
          st != 0 ? "refused" : "NOT REFUSED");
  if (st == 0) {
    s_errors++;
  }
  (void)prv_cmd("updater_reboot", 0xfc21U, NULL, 0U, r, &n);
  if (prv_self_restart() != 1) {
    s_errors++;
  }

  tprintf("a 5 ms reset with the lines driven:\n");
  s_reset_ms = 5U;
  prv_reset();
  const uint8_t reason = s_pkt[5];
  tprintf("  reason %u: %s\n", reason, reason == 4U ? "updater, by the boot pin" : "not the updater");
  if (reason != 4U) {
    s_errors++;
  }
  (void)prv_cmd("updater_reboot", 0xfc21U, NULL, 0U, r, &n);
  if (prv_self_restart() != 1) {
    s_errors++;
    tprintf("  recovery reason %d\n", prv_long_restart());
  }
}

#define WRITE_MAGIC 0x57524e42U /* "BNRW" */
#define PAGE_LOW    0x10042800U
#define PAGE_HIGH   0x1007f000U

static void prv_write_pages(const volatile uint32_t *t, bool official) {
  uint8_t r[260];
  uint32_t n;
  int st;

  if (t[0] != WRITE_MAGIC || t[1] == 0U || t[1] > 16U) {
    s_errors++;
    tprintf("bad table\n");
    return;
  }
  const uint32_t count = t[1];
  for (uint32_t i = 0U; i < count; i++) {
    const uint32_t a = t[2U + i * 3U];
    const volatile uint8_t *src = (const volatile uint8_t *)t[3U + i * 3U];
    /* The official flow may rewrite the DTM's first page, its BLUE flag left blank for 0xfc25 */
    if (official && a == FLASH_DTM && src[0x14] == 0xffU && src[0x15] == 0xffU &&
        src[0x16] == 0xffU && src[0x17] == 0xffU) {
      continue;
    }
    if (a < PAGE_LOW || a >= PAGE_HIGH || (a & (FLASH_PAGE - 1U)) != 0U) {
      s_errors++;
      tprintf("page %08lx refused: outside 0x10042800..0x1007efff or not a page\n",
              (unsigned long)a);
      return;
    }
  }
  if (prv_enter_updater() != 0) {
    s_errors++;
    tprintf("  no updater; recovery reason %d\n", prv_long_restart());
    return;
  }

  if (official) {
    /* As ST's updater tools do: the flag cleared first, so that an interrupted update starts the
       updater (reason 3) rather than a part-written DTM */
    st = prv_cmd("updater_erase_blueflag", 0xfc24U, NULL, 0U, r, &n);
    tprintf("  erase BLUE flag: status %d\n", st);
    if (st != 0) {
      s_errors++;
      return;
    }
  }

  for (uint32_t i = 0U; i < count; i++) {
    const uint32_t a = t[2U + i * 3U];
    const volatile uint8_t *src = (const volatile uint8_t *)t[3U + i * 3U];
    const uint32_t flags = t[4U + i * 3U];
    uint8_t par[6 + READ_BLOCK];

    tprintf("page %08lx:%s%s\n", (unsigned long)a, (flags & 1U) ? " erase" : "",
            (flags & 2U) ? " program" : "");
    if (flags & 1U) {
      prv_put_le32(par, a);
      st = prv_cmd("updater_erase_sector", 0xfc26U, par, 4U, r, &n);
      if (st != 0) {
        s_errors++;
        tprintf("  erase: status %d; stopping, in the updater\n", st);
        return;
      }
    }
    s_quiet = true;
    for (uint32_t off = 0U; (flags & 2U) && off < FLASH_PAGE; off += READ_BLOCK) {
      bool blank = true;
      for (uint32_t k = 0U; k < READ_BLOCK; k++) {
        par[6 + k] = src[off + k];
        blank = blank && par[6 + k] == 0xffU;
      }
      if (blank) {
        continue;
      }
      prv_put_le32(par, a + off);
      par[4] = (uint8_t)READ_BLOCK;
      par[5] = 0U;
      st = prv_cmd("updater_prog_data_block", 0xfc27U, par, 6U + READ_BLOCK, r, &n);
      if (st != 0) {
        s_quiet = false;
        s_errors++;
        tprintf("  program %08lx: status %d; stopping, in the updater\n",
                (unsigned long)(a + off), st);
        return;
      }
    }
    uint32_t diff = 0U;
    for (uint32_t off = 0U; off < FLASH_PAGE; off += READ_BLOCK) {
      prv_put_le32(par, a + off);
      par[4] = (uint8_t)READ_BLOCK;
      par[5] = 0U;
      st = prv_cmd("updater_read_data_block", 0xfc28U, par, 6U, r, &n);
      for (uint32_t k = 0U; k < READ_BLOCK; k++) {
        const uint8_t want = (flags & 2U) ? src[off + k] : 0xffU;
        if (st != 0 || r[1 + k] != want) {
          diff++;
        }
      }
    }
    s_quiet = false;
    prv_put_le32(par, a);
    par[4] = 1U;
    st = prv_cmd("updater_calc_crc", 0xfc29U, par, 5U, r, &n);
    const uint32_t chip = st == 0 ? prv_le32(&r[1]) : 0U;
    uint32_t want = 0U;
    if (flags & 2U) {
      want = prv_crc(src, FLASH_PAGE);
    } else {
      static const uint8_t s_ff[4] = {0xffU, 0xffU, 0xffU, 0xffU};
      /* An erased page: the CRC of 2048 bytes of 0xff */
      uint32_t crc = 0U;
      for (uint32_t k = 0U; k < FLASH_PAGE; k += 4U) {
        crc ^= prv_le32(s_ff);
        for (int j = 0; j < 32; j++) {
          crc = (crc << 1) ^ ((crc >> 31) != 0U ? 0x04c11db7U : 0U);
        }
      }
      want = crc;
    }
    tprintf("  read back: %lu bytes differ; crc %08lx, expected %08lx\n", (unsigned long)diff,
            (unsigned long)chip, (unsigned long)want);
    if (diff != 0U || st != 0 || chip != want) {
      s_errors++;
      tprintf("  stopping, in the updater\n");
      return;
    }
  }

  if (official) {
    st = prv_cmd("updater_reset_blueflag", 0xfc25U, NULL, 0U, r, &n);
    uint8_t par[6];
    prv_put_le32(par, FLASH_DTM + 0x14U);
    par[4] = 4U;
    par[5] = 0U;
    const int st2 = prv_cmd("updater_read_data_block", 0xfc28U, par, 6U, r, &n);
    const uint32_t flag = st2 == 0 ? prv_le32(&r[1]) : 0U;
    tprintf("  set BLUE flag: status %d, flag now %08lx\n", st, (unsigned long)flag);
    if (st != 0 || flag != 0x424c5545U) {
      s_errors++;
      tprintf("  stopping, in the updater\n");
      return;
    }
  }

  (void)prv_cmd("updater_reboot", 0xfc21U, NULL, 0U, r, &n);
  if (prv_self_restart() != 1) {
    s_errors++;
    tprintf("  not a normal start after the write\n");
    return;
  }
  prv_aci_versions();
}

static void prv_probe(uint32_t hold_ms) {
  uint8_t r[260];
  uint32_t n;

  prv_set(RST_PORT, RST_PIN, 0U);
  prv_lines_released(false);
  prv_analog(MISO_PORT, MISO_PIN);
  tprintf("RESET low %lu ms, all lines analog\n", (unsigned long)hold_ms);
  for (uint32_t t = 0U; t < hold_ms; t += 500U) {
    tether->watchdog_feed();
    tether_delay_ms(500U);
  }
  prv_in(MISO_PORT, MISO_PIN);
  prv_lines_driven();
  tether_delay_ms(5U);
  prv_set(RST_PORT, RST_PIN, 1U);
  tprintf("released; IRQ, MISO, RESET every 100 ms:");
  for (int i = 0; i < 50; i++) {
    tether->watchdog_feed();
    if (i % 10 == 0) {
      tprintf("\n ");
    }
    tprintf(" %lu%lu%lu", (unsigned long)prv_irq(), (unsigned long)prv_get(MISO_PORT, MISO_PIN),
            (unsigned long)prv_get(RST_PORT, RST_PIN));
    if (prv_irq()) {
      break;
    }
    tether_delay_ms(100U);
  }
  tprintf("\n");
  if (prv_irq()) {
    const int k = prv_read(s_pkt, sizeof(s_pkt), true);
    prv_dump("  packet", s_pkt, (uint32_t)k);
  }
  (void)prv_cmd("hci_read_local_version_information", 0x1001U, NULL, 0U, r, &n);
  (void)prv_cmd("updater_get_version", 0xfc22U, NULL, 0U, r, &n);
}

uint32_t blob_main(const uint32_t *args, uint32_t nargs) {
  const uint32_t mode = nargs > 0U ? args[0] : 0U;
  const uint32_t adv_s = nargs > 1U && mode < 3U ? (args[1] > 10U ? 10U : args[1]) : 0U;

  REG32(RCC_AHB2ENR1) |= (1U << PA) | (1U << PB) | (1U << PG);
  (void)REG32(RCC_AHB2ENR1);

  prv_save(&s_save[0], SCK_PORT, SCK_PIN);
  prv_save(&s_save[1], MISO_PORT, MISO_PIN);
  prv_save(&s_save[2], MOSI_PORT, MOSI_PIN);
  prv_save(&s_save[3], CS_PORT, CS_PIN);
  prv_save(&s_save[4], IRQ_PORT, IRQ_PIN);
  prv_save(&s_save[5], RST_PORT, RST_PIN);
  const bool was_in_reset = s_save[5].moder == 1U && s_save[5].odr == 0U;
  tprintf("bluenrg2 mode %lu: RESET was %s (mode %lu, level %lu), IRQ %lu\n",
          (unsigned long)mode, was_in_reset ? "held low" : "not driven low",
          (unsigned long)s_save[5].moder, (unsigned long)prv_get(RST_PORT, RST_PIN),
          (unsigned long)prv_irq());

  prv_out(CS_PORT, CS_PIN, false, 1U);
  prv_out(SCK_PORT, SCK_PIN, false, 0U);
  prv_out(MOSI_PORT, MOSI_PIN, false, 0U);
  prv_in(MISO_PORT, MISO_PIN);
  prv_in(IRQ_PORT, IRQ_PIN);
  prv_out(RST_PORT, RST_PIN, true, 0U);

  if (mode == 3U) {
    tprintf("reset with MISO pulled up:\n");
    REG32(GPIO_PORT(MISO_PORT) + 0x0cU) |= 1U << (MISO_PIN * 2U);
    prv_reset();
    REG32(GPIO_PORT(MISO_PORT) + 0x0cU) &= ~(3U << (MISO_PIN * 2U));
    tprintf("reset with MISO floating:\n");
    prv_reset();
    prv_versions();
  } else if (mode == 5U) {
    static const uint32_t s_ms[6] = {5U, 20U, 100U, 300U, 1000U, 3000U};
    for (int i = 0; i < 6; i++) {
      s_reset_ms = 3000U;
      tprintf("normal start first:\n");
      prv_reset();
      s_reset_ms = s_ms[i];
      tprintf("reset held %lu ms:\n", (unsigned long)s_reset_ms);
      prv_reset();
    }
  } else if (mode == 6U) {
    const uint32_t mask = nargs > 1U ? args[1] : 0x1fU;
    s_reset_ms = 3000U;
    prv_reset();
    s_reset_ms = 5U;
    prv_reset();
    tprintf("hold 2 s, pins %02lx as found:\n", (unsigned long)mask);
    prv_set(RST_PORT, RST_PIN, 0U);
    for (int i = 0; i < 5; i++) {
      if (mask & (1U << i)) {
        prv_restore(&s_save[i]);
      }
    }
    tether_delay_ms(2000U);
    prv_out(CS_PORT, CS_PIN, false, 1U);
    prv_out(SCK_PORT, SCK_PIN, false, 0U);
    prv_out(MOSI_PORT, MOSI_PIN, false, 0U);
    prv_in(MISO_PORT, MISO_PIN);
    prv_in(IRQ_PORT, IRQ_PIN);
    prv_reset();
  } else if (mode == 7U) {
    static const uint8_t s_ll_only[3] = {0x2cU, 0x01U, 0x01U};
    static const uint8_t s_off = 0x2cU;
    const uint32_t hold = nargs > 1U && args[1] != 0U ? (args[1] > 3000U ? 3000U : args[1]) : 5U;
    uint8_t r[260];
    uint32_t n;

    for (int run = 0; run < 2 && s_errors == 0; run++) {
      const uint32_t ms = run == 0 ? 2000U : hold;

      tprintf("restart from %lu ms in reset, SCK MOSI CS analog:\n", (unsigned long)ms);
      prv_set(RST_PORT, RST_PIN, 0U);
      prv_analog(SCK_PORT, SCK_PIN);
      prv_analog(MOSI_PORT, MOSI_PIN);
      prv_analog(CS_PORT, CS_PIN);
      tether_delay_ms(ms);
      prv_out(CS_PORT, CS_PIN, false, 1U);
      prv_out(SCK_PORT, SCK_PIN, false, 0U);
      prv_out(MOSI_PORT, MOSI_PIN, false, 0U);
      prv_reset();
      if (s_errors != 0 || s_pkt[5] != 1U) {
        break;
      }
      tprintf("  config 0x2c:\n");
      (void)prv_cmd("aci_hal_read_config_data", 0xfc0dU, &s_off, 1U, r, &n);
      if (run == 0) {
        tprintf("  status %d\n",
                prv_cmd("aci_hal_write_config_data (ll only)", 0xfc0cU, s_ll_only, 3U, r, &n));
        (void)prv_cmd("aci_hal_read_config_data", 0xfc0dU, &s_off, 1U, r, &n);
      }
    }
    prv_versions();
  } else if (mode == 12U) {
    prv_probe(nargs > 1U && args[1] != 0U ? args[1] : 10000U);
  } else if (mode == 10U) {
    prv_checks();
  } else if (mode == 11U) {
    prv_write_pages((const volatile uint32_t *)(nargs > 1U ? args[1] : 0U), false);
  } else if (mode == 13U) {
    prv_write_pages((const volatile uint32_t *)(nargs > 1U ? args[1] : 0U), true);
  } else if (mode == 9U) {
    prv_backup(nargs > 1U && args[1] != 0U ? args[1] : 0x20100000U);
  } else if (mode == 8U) {
    static const uint8_t s_ll_only[3] = {0x2cU, 0x01U, 0x01U};
    static const uint8_t s_off = 0x2cU;
    const uint32_t flags = nargs > 1U ? args[1] : 0U;
    uint8_t r[260];
    uint32_t n;

    tprintf("normal start from 2000 ms in reset, SCK MOSI CS analog:\n");
    prv_set(RST_PORT, RST_PIN, 0U);
    prv_analog(SCK_PORT, SCK_PIN);
    prv_analog(MOSI_PORT, MOSI_PIN);
    prv_analog(CS_PORT, CS_PIN);
    tether_delay_ms(2000U);
    prv_out(CS_PORT, CS_PIN, false, 1U);
    prv_out(SCK_PORT, SCK_PIN, false, 0U);
    prv_out(MOSI_PORT, MOSI_PIN, false, 0U);
    prv_reset();
    if (s_errors == 0 && s_pkt[5] == 1U) {
      if (flags & 2U) {
        tprintf("  status %d\n",
                prv_cmd("aci_hal_write_config_data (ll only)", 0xfc0cU, s_ll_only, 3U, r, &n));
      }
      (void)prv_cmd("aci_hal_read_config_data", 0xfc0dU, &s_off, 1U, r, &n);
      const int st = prv_cmd("hci_reset", 0x0c03U, NULL, 0U, r, &n);
      prv_analog(SCK_PORT, SCK_PIN);
      prv_analog(MOSI_PORT, MOSI_PIN);
      if ((flags & 1U) == 0U) {
        prv_analog(CS_PORT, CS_PIN);
      }
      const uint32_t t0 = tether->millis();
      tprintf("  hci_reset status %d, lines released (CS %s)\n", st,
              (flags & 1U) ? "kept high" : "analog");
      bool up = false;
      while (!up && tether->millis() - t0 < 3000U) {
        tether->watchdog_feed();
        up = prv_irq();
      }
      const uint32_t t_up = tether->millis() - t0;
      prv_out(CS_PORT, CS_PIN, false, 1U);
      prv_out(SCK_PORT, SCK_PIN, false, 0U);
      prv_out(MOSI_PORT, MOSI_PIN, false, 0U);
      int reason = -1;
      if (up) {
        const int k = prv_read(s_pkt, sizeof(s_pkt), true);
        prv_dump("  start event", s_pkt, (uint32_t)k);
        if (k >= 6 && s_pkt[1] == 0xffU && s_pkt[3] == 0x01U && s_pkt[4] == 0x00U) {
          reason = s_pkt[5];
        }
      }
      tprintf("  restart: IRQ %s after %lu ms, reason %d\n", up ? "high" : "never high",
              (unsigned long)t_up, reason);
      if (reason == 1) {
        (void)prv_cmd("aci_hal_read_config_data", 0xfc0dU, &s_off, 1U, r, &n);
        tprintf("  status %d\n",
                prv_cmd("aci_hal_write_config_data (ll only)", 0xfc0cU, s_ll_only, 3U, r, &n));
        (void)prv_cmd("aci_hal_read_config_data", 0xfc0dU, &s_off, 1U, r, &n);
        prv_versions();
      } else {
        s_errors++;
        tprintf("recovery: 2000 ms in reset, SCK MOSI CS analog:\n");
        prv_set(RST_PORT, RST_PIN, 0U);
        prv_analog(SCK_PORT, SCK_PIN);
        prv_analog(MOSI_PORT, MOSI_PIN);
        prv_analog(CS_PORT, CS_PIN);
        tether_delay_ms(2000U);
        prv_out(CS_PORT, CS_PIN, false, 1U);
        prv_out(SCK_PORT, SCK_PIN, false, 0U);
        prv_out(MOSI_PORT, MOSI_PIN, false, 0U);
        prv_reset();
        prv_versions();
      }
    }
  } else if (mode == 4U) {
    REG32(GPIO_PORT(MISO_PORT) + 0x0cU) |= 2U << (MISO_PIN * 2U);
    for (int i = 0; i < 3; i++) {
      tprintf("reset %d with MISO pulled down:\n", i + 1);
      prv_reset();
    }
    prv_simple("hci_reset", 0x0c03U);
    tprintf("reset with MISO pulled down:\n");
    prv_reset();
    prv_versions();
    REG32(GPIO_PORT(MISO_PORT) + 0x0cU) &= ~(3U << (MISO_PIN * 2U));
  } else {
    prv_reset();
  }
  if (s_errors == 0 && mode < 3U) {
    uint8_t r[260];
    uint32_t n;
    if (mode == 1U) {
      static const uint8_t s_ll_only[3] = {0x2cU, 0x01U, 0x01U};
      tprintf("  status %d\n",
              prv_cmd("aci_hal_write_config_data (ll only)", 0xfc0cU, s_ll_only, 3U, r, &n));
    }
    prv_versions();
    if (mode != 2U) {
      prv_aci_versions();
    }
    prv_hci_queries();
    if (adv_s != 0U) {
      prv_advertise(adv_s);
    }
    /* Last: hci_reset restarts the chip, which then answers nothing */
    prv_simple("hci_reset", 0x0c03U);
    prv_versions();
  }

  /* Back in reset if it was, else restarted as the stock firmware leaves it */
  if (was_in_reset) {
    prv_set(RST_PORT, RST_PIN, 0U);
  }
  for (int i = 0; i < 6; i++) {
    prv_restore(&s_save[i]);
  }
  tprintf("%d errors\n", s_errors);
  return (uint32_t)s_errors;
}
