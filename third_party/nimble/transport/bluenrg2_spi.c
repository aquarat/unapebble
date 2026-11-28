/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * A transaction takes the chip select low, waits for IRQ high, exchanges a 5-byte header, 0x0a to
 * write or 0x0b to read, which gives the room in the chip's write buffer and the bytes it has to
 * read, moves the packet, waits for IRQ low and raises the chip select. The chip raises IRQ by
 * itself when it has data.
 *
 * The chip's quirks, measured on the UNA Watch (docs/boards/unawatch/hardware.md, 2.5.1): a reset
 * soon after a start, with the SPI lines driven, starts its updater, where it answers no command.
 * A start is checked by the reason of the vendor event it begins with, and retried with the chip
 * held in reset for 2 s first, its chip select undriven. HCI_Reset is never sent.
 */

#include "bluenrg2_spi.h"

#include <errno.h>
#include <string.h>

#include <pbl/drivers/bt_hci/bluenrg2.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/intc.h>
#include <pbl/drivers/spi.h>
#include <pbl/kernel/assert.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/sem.h>
#include <pbl/kernel/thread.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>

PBL_LOG_MODULE_DEFINE(bluenrg2_spi, CONFIG_BLUENRG2_SPI_LOG_LEVEL);

#define HDR_LEN   5U
#define HDR_WRITE 0x0aU
#define HDR_READ  0x0bU

// Up to 1.82 ms from sleep (DS13280), with a margin
#define IRQ_HIGH_TIMEOUT_MS 10U
// IRQ falls during the header, or soon after
#define IRQ_LOW_SPIN_US    200U
#define IRQ_LOW_TIMEOUT_MS 10U
#define IRQ_POLL_US        10U
// The wakeup controller misses a chip select taken within about 30 us of the chip going to sleep
#define TX_GAP_US        100U
#define WRITE_TIMEOUT_MS 1000U

#define RESET_HOLD_MS    2000U
#define RESET_PULSE_MS   5U
#define START_TIMEOUT_MS 3000U
#define START_TRIES      3U
#define CMD_TIMEOUT_MS   1000U

#define EVT_COMMAND_COMPLETE 0x0eU
#define EVT_COMMAND_STATUS   0x0fU
#define EVT_VS               0xffU

#define OP_ACI_READ_CONFIG 0xfc0dU
#define CONFIG_RANDOM_ADDR 0x80U

#define INIT_BUF_SIZE 512U
// The most a read takes at once
#define READ_MAX 512U

#define NRG PBL_CONTAINER_OF(BT_HCI, const struct pbl_bt_hci_bluenrg2, hci)

// IRQ rose, or the thread was kicked
static PBL_SEM_DEFINE(s_wake, 0, 1);
// IRQ rose
static PBL_SEM_DEFINE(s_irq_rose, 0, 1);
static struct pbl_irq_spec s_irq;
static uint8_t s_ff[READ_MAX];

// Masked until the thread has seen to it, so that a line that toggles cannot hold the CPU
static void prv_irq_handler(void *arg) {
  (void)pbl_intc_disable(&s_irq);
  pbl_sem_give(&s_irq_rose);
  pbl_sem_give(&s_wake);
}

static void prv_irq_arm(void) {
  (void)pbl_intc_enable(&s_irq);
}

bool bnrg_irq_high(void) {
  return pbl_gpio_get(&NRG->irq) > 0;
}

void bnrg_wait(void) {
  prv_irq_arm();
  if (!bnrg_irq_high()) {
    (void)pbl_sem_take(&s_wake, PBL_FOREVER);
  }
}

void bnrg_kick(void) {
  pbl_sem_give(&s_wake);
}

static bool prv_wait_irq_low(void) {
  for (uint32_t us = 0U; us < IRQ_LOW_SPIN_US; us += IRQ_POLL_US) {
    if (!bnrg_irq_high()) {
      return true;
    }
    pbl_busy_wait_us(IRQ_POLL_US);
  }

  for (uint32_t ms = 0U; ms < IRQ_LOW_TIMEOUT_MS; ms++) {
    pbl_sleep_ms(1U);
    if (!bnrg_irq_high()) {
      return true;
    }
  }

  return false;
}

static int prv_xfer(const void *tx, void *rx, uint32_t len) {
  struct pbl_spi_xfer xfer = {.tx_buf = tx, .rx_buf = rx, .len = len};

  return pbl_spi_transfer(&NRG->spi, &xfer, 1U);
}

int bnrg_read(uint8_t *buf, uint32_t size) {
  static const uint8_t s_hdr[HDR_LEN] = {HDR_READ, 0U, 0U, 0U, 0U};
  uint8_t hdr[HDR_LEN];
  uint32_t len;
  int ret;

  ret = pbl_spi_hold(&NRG->spi);
  if (ret != 0) {
    return ret;
  }

  ret = prv_xfer(s_hdr, hdr, HDR_LEN);
  len = (uint32_t)hdr[3] | ((uint32_t)hdr[4] << 8);
  // A floating MISO reads all ones
  if (ret != 0 || len == 0xffffU) {
    len = 0U;
  }
  len = PBL_MIN(len, PBL_MIN(size, (uint32_t)sizeof(s_ff) - 1U));
  // One byte more than there is, dropped: the last byte of a transfer comes back with only its two
  // low bits
  if (len > 0U) {
    static uint8_t s_rx[READ_MAX];

    ret = prv_xfer(s_ff, s_rx, len + 1U);
    memcpy(buf, s_rx, len);
  }

  // Raising the chip select with IRQ still high sends the chip down an error path
  bool low = prv_wait_irq_low();
  pbl_spi_release(&NRG->spi);

  if (!low || len == 0U) {
    static uint32_t s_odd_reads;

    if (s_odd_reads++ < 20U) {
      PBL_LOG_WRN("Read: header %02x %02x %02x %02x %02x, IRQ %s", hdr[0], hdr[1], hdr[2], hdr[3],
                  hdr[4], low ? "low" : "stays high");
    }
    // Not to spin on a line stuck high
    pbl_sleep_ms(1U);
  }

  return ret != 0 ? ret : (int)len;
}

int bnrg_write(const uint8_t *buf, uint32_t len, void (*rx_cb)(void)) {
  static const uint8_t s_hdr[HDR_LEN] = {HDR_WRITE, 0U, 0U, 0U, 0U};
  uint64_t start = pbl_uptime_ms();
  uint8_t hdr[HDR_LEN];

  while (pbl_uptime_ms() - start < WRITE_TIMEOUT_MS) {
    bool ready = false;
    bool sent = false;
    int ret;

    if (bnrg_irq_high()) {
      rx_cb();
      continue;
    }

    (void)pbl_sem_take(&s_irq_rose, PBL_NO_WAIT);
    prv_irq_arm();
    ret = pbl_spi_hold(&NRG->spi);
    if (ret != 0) {
      return ret;
    }

    ready = bnrg_irq_high() ||
            (pbl_sem_take(&s_irq_rose, PBL_MSEC(IRQ_HIGH_TIMEOUT_MS)) == 0 && bnrg_irq_high());
    if (ready) {
      uint32_t wbuf;

      ret = prv_xfer(s_hdr, hdr, HDR_LEN);
      wbuf = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8);
      if (ret == 0 && wbuf != 0xffffU && wbuf >= len) {
        ret = prv_xfer(buf, nullptr, len);
        sent = (ret == 0);
      }
      (void)prv_wait_irq_low();
    }
    pbl_spi_release(&NRG->spi);
    pbl_busy_wait_us(TX_GAP_US);

    if (sent) {
      return 0;
    }
    if (!ready) {
      // Busy with the radio, or with data the host has not read: read it, if any, and retry
      pbl_sleep_ms(1U);
    }
  }

  return -ETIMEDOUT;
}

// --- Start-up ----------------------------------------------------------------------------------

// Packets read during the start-up, a whole H4 one at a time
static uint8_t s_init_pkt[INIT_BUF_SIZE];
static uint8_t s_init_tx[INIT_BUF_SIZE];
static uint32_t s_init_len;

static void prv_init_rx(void) {
  int n = bnrg_read(s_init_pkt, sizeof(s_init_pkt));

  s_init_len = n > 0 ? (uint32_t)n : 0U;
}

static bool prv_wait_irq_high(uint32_t timeout_ms) {
  uint64_t start = pbl_uptime_ms();

  while (!bnrg_irq_high()) {
    uint64_t waited = pbl_uptime_ms() - start;

    if (waited >= timeout_ms) {
      return false;
    }
    prv_irq_arm();
    if (bnrg_irq_high()) {
      break;
    }
    (void)pbl_sem_take(&s_irq_rose, PBL_MSEC((uint32_t)(timeout_ms - waited)));
  }

  return true;
}

int bnrg_init_cmd(uint16_t op, const void *par, uint8_t par_len, void *ret, size_t *ret_len) {
  uint64_t start;
  int rc;

  s_init_tx[0] = BNRG_PKT_CMD;
  s_init_tx[1] = (uint8_t)op;
  s_init_tx[2] = (uint8_t)(op >> 8);
  s_init_tx[3] = par_len;
  if (par_len > 0U) {
    memcpy(&s_init_tx[4], par, par_len);
  }

  s_init_len = 0U;
  rc = bnrg_write(s_init_tx, 4U + par_len, prv_init_rx);
  if (rc != 0) {
    return rc;
  }

  start = pbl_uptime_ms();
  while (pbl_uptime_ms() - start < CMD_TIMEOUT_MS) {
    if (s_init_len == 0U) {
      if (!prv_wait_irq_high(CMD_TIMEOUT_MS)) {
        break;
      }
      prv_init_rx();
    }

    const uint8_t *p = s_init_pkt;
    uint32_t n = s_init_len;

    s_init_len = 0U;
    if (n >= 7U && p[0] == BNRG_PKT_EVT && p[1] == EVT_COMMAND_COMPLETE &&
        (uint16_t)(p[4] | (p[5] << 8)) == op) {
      size_t len = PBL_MIN((size_t)p[2] - 3U, n - 6U);

      if (ret != nullptr) {
        *ret_len = PBL_MIN(*ret_len, len - 1U);
        memcpy(ret, &p[7], *ret_len);
      }
      return p[6];
    }
    if (n >= 7U && p[0] == BNRG_PKT_EVT && p[1] == EVT_COMMAND_STATUS &&
        (uint16_t)(p[5] | (p[6] << 8)) == op) {
      return p[3];
    }
    PBL_LOG_DBG("Start-up: packet %02x %02x dropped", n > 0U ? p[0] : 0U, n > 1U ? p[1] : 0U);
  }

  return -ETIMEDOUT;
}

// The start event's reason, or a negative errno
static int prv_start_event(void) {
  if (!prv_wait_irq_high(START_TIMEOUT_MS)) {
    return -ETIMEDOUT;
  }

  prv_init_rx();
  if (s_init_len >= 6U && s_init_pkt[0] == BNRG_PKT_EVT && s_init_pkt[1] == EVT_VS &&
      (s_init_pkt[3] | (s_init_pkt[4] << 8)) == BNRG_VS_BLUE_INITIALIZED) {
    return s_init_pkt[5];
  }

  return -EIO;
}

void bnrg_hold_reset(void) {
  const struct pbl_bt_hci_bluenrg2 *nrg = NRG;
  const struct pbl_gpio *cs = &nrg->spi.bus->cs_gpios[nrg->spi.cs];

  (void)pbl_gpio_set(&nrg->reset, true);
  if (pbl_gpio_is_connected(cs)) {
    (void)pbl_gpio_configure(cs, PBL_GPIO_INPUT);
  }
}

// Held in reset with the chip select undriven, then started: started normally or not
static int prv_restart(uint32_t hold_ms) {
  const struct pbl_bt_hci_bluenrg2 *nrg = NRG;
  int ret;

  bnrg_hold_reset();
  pbl_sleep_ms(PBL_MAX(hold_ms, RESET_PULSE_MS));

  // The chip select high from here: low while the chip starts can start its updater
  ret = pbl_spi_setup(&nrg->spi);
  if (ret != 0) {
    return ret;
  }
  pbl_sleep_ms(RESET_PULSE_MS);

  (void)pbl_sem_take(&s_irq_rose, PBL_NO_WAIT);
  (void)pbl_gpio_set(&nrg->reset, false);

  return prv_start_event();
}

int bnrg_read_identity(struct pbl_bt_addr *addr) {
  static const uint8_t s_off = CONFIG_RANDOM_ADDR;
  uint8_t rsp[1U + sizeof(addr->octets)];
  size_t len = sizeof(rsp);
  uint8_t ones = 0xffU;
  uint8_t any = 0U;
  int rc;

  rc = bnrg_init_cmd(OP_ACI_READ_CONFIG, &s_off, 1U, rsp, &len);
  if (rc != 0 || len != sizeof(rsp) || rsp[0] != sizeof(addr->octets)) {
    PBL_LOG_WRN("No address in the chip's configuration (%d)", rc);
    return -ENOENT;
  }

  memcpy(addr->octets, &rsp[1], sizeof(addr->octets));
  addr->octets[5] |= 0xc0U;
  for (size_t i = 0U; i < sizeof(addr->octets); i++) {
    uint8_t random = i == 5U ? (addr->octets[i] & 0x3fU) : addr->octets[i];

    any |= random;
    ones &= i == 5U ? (random | 0xc0U) : random;
  }
  // Its random part all zeros or all ones is no address
  if (any == 0U || ones == 0xffU) {
    PBL_LOG_WRN("No address in the chip's configuration");
    return -ENOENT;
  }

  PBL_LOG_DBG("Identity address %02x:%02x:%02x:%02x:%02x:%02x", addr->octets[5], addr->octets[4],
              addr->octets[3], addr->octets[2], addr->octets[1], addr->octets[0]);

  return 0;
}

void bnrg_bring_up(bool first, int (*setup)(void)) {
  uint32_t tries = 0U;

  while (true) {
    uint64_t up_ms = pbl_uptime_ms();
    // At boot the chip has been in reset since its device came up
    uint32_t hold = (first && tries == 0U && up_ms < RESET_HOLD_MS)
                        ? (uint32_t)(RESET_HOLD_MS - up_ms)
                        : (first && tries == 0U ? RESET_PULSE_MS : RESET_HOLD_MS);
    int reason = prv_restart(hold);

    tries++;
    if (reason == BNRG_START_REASON_NORMAL && setup() == 0) {
      return;
    }

    PBL_LOG_WRN("BlueNRG-2 start %lu failed (reason %d), restarting", (unsigned long)tries, reason);
    if (tries >= START_TRIES) {
      // Not worth stopping the watch for: try now and then
      pbl_sleep_ms(10U * RESET_HOLD_MS);
      tries = 0U;
    }
    first = false;
  }
}

void bnrg_spi_init(void) {
  int ret;

  memset(s_ff, 0xff, sizeof(s_ff));

  PBL_ASSERTN(pbl_device_is_ready(&BT_HCI->dev));

  ret = pbl_gpio_to_irq(&NRG->irq, PBL_IRQ_TYPE_EDGE_RISING, &s_irq);
  if (ret == 0) {
    ret = pbl_device_init(&s_irq.intc->dev);
  }
  if (ret == 0) {
    ret = pbl_intc_request(&s_irq, prv_irq_handler, nullptr);
  }
  if (ret == 0) {
    ret = pbl_intc_enable(&s_irq);
  }
  PBL_ASSERT(ret == 0, "BlueNRG-2 IRQ setup failed (%d)", ret);
}
