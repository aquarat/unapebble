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
 *   args[1] != 0 also advertise for args[1] seconds with standard HCI commands,
 *                non-connectable, as "unapebble-test"
 *
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
  tprintf("> %s (%04x)\n", name, op);
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
    prv_dump("<", s_pkt, (uint32_t)n);
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

static void prv_reset(void) {
  prv_set(CS_PORT, CS_PIN, 1U);
  prv_set(RST_PORT, RST_PIN, 0U);
  tether_delay_ms(5U);
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

uint32_t blob_main(const uint32_t *args, uint32_t nargs) {
  const uint32_t mode = nargs > 0U ? args[0] : 0U;
  const uint32_t adv_s = nargs > 1U ? (args[1] > 10U ? 10U : args[1]) : 0U;

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

  prv_reset();
  if (s_errors == 0) {
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
