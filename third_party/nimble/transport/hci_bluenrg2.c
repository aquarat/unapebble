/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * HCI to an ST BlueNRG-2 running ST's DTM firmware (SPI variant), in link-layer-only mode, over
 * its SPI protocol (BlueNRG-2N data sheet DS13280, 6.5). A transaction takes the chip select low,
 * waits for IRQ high, exchanges a 5-byte header, 0x0a to write or 0x0b to read, which gives the
 * room in the chip's write buffer and the bytes it has to read, moves the packet, waits for IRQ
 * low and raises the chip select. The packets are H4 ones. The chip raises IRQ by itself when it
 * has data. One thread does every transaction: the host's packets are queued to it.
 *
 * The chip's quirks, measured on the UNA Watch (docs/boards/unawatch/hardware.md, 2.5.1):
 * HCI_Reset, or a reset soon after a start, starts its updater, where it answers no command. The
 * host's HCI_Reset is answered here and never sent. A start is checked by the reason of the
 * vendor event it begins with, and retried with the chip held in reset for 2 s first, its SPI
 * lines undriven. Link-layer-only mode, which hands ACL data to the host, is set after each start;
 * it is lost at the next. The chip has no public address: it reports a placeholder, hidden from
 * the host, and the module's static random address is the identity address.
 */

#include "hci_alloc.h"

#include <errno.h>
#include <string.h>

#include <pbl/bluetooth/id_addr.h>
#include <pbl/drivers/bt_hci/bluenrg2.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/intc.h>
#include <pbl/drivers/spi.h>
#include <pbl/kernel/assert.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/mutex.h>
#include <pbl/kernel/sem.h>
#include <pbl/kernel/thread.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

// clang-format off
#include <os/os_mbuf.h>
// clang-format on
#include <nimble/hci_common.h>
#include <nimble/nimble_port_pebble.h>
#include <nimble/transport.h>
#include <nimble/transport/hci_h4.h>
#include <nimble/transport_impl.h>
#include <os/endian.h>

PBL_LOG_MODULE_DEFINE(hci_bluenrg2, CONFIG_NIMBLE_HCI_BLUENRG2_LOG_LEVEL);

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

// aci_blue_initialized_event, and its reasons
#define VS_BLUE_INITIALIZED 0x0001U
#define START_REASON_NORMAL 0x01U

#define OP_HCI_RESET        0x0c03U
#define OP_HCI_READ_BD_ADDR 0x1009U
#define OP_ACI_WRITE_CONFIG 0xfc0cU
#define OP_ACI_READ_CONFIG  0xfc0dU
#define OP_ACI_FW_DETAILS   0xfc01U
#define OP_ACI_GAP_INIT     0xfc8aU
#define OP_ACI_GATT_INIT    0xfd01U

#define CONFIG_LL_ONLY     0x2cU
#define CONFIG_RANDOM_ADDR 0x80U

#define RX_BUF_SIZE 512U
// An ACL packet of 251 bytes with its headers, or a command of 255
#define TX_BUF_SIZE  (1U + 4U + 255U)
#define TX_QUEUE_LEN 16U

#define NRG PBL_CONTAINER_OF(BT_HCI, const struct pbl_bt_hci_bluenrg2, hci)

struct tx_entry {
  uint8_t type;
  void *buf;
};

static struct pbl_thread s_hci_thread;
PBL_THREAD_STACK_DEFINE(s_hci_task_stack, CONFIG_NIMBLE_HCI_THREAD_STACK_SIZE);
// IRQ rose, or the host queued something
static PBL_SEM_DEFINE(s_wake, 0, 1);
// IRQ rose
static PBL_SEM_DEFINE(s_irq_rose, 0, 1);
static PBL_MUTEX_DEFINE(s_queue_lock);
static struct hci_h4_sm s_hci_h4sm;
static struct pbl_irq_spec s_irq;

static struct tx_entry s_queue[TX_QUEUE_LEN];
static uint32_t s_queue_head;
static uint32_t s_queue_len;
// The host asked for a reset, answered once the chip is up
static volatile bool s_reset_pending;
// The chip has started and is set up; the host's packets go to it
static bool s_up;

static uint8_t s_rx_buf[RX_BUF_SIZE];
static uint8_t s_tx_buf[TX_BUF_SIZE];
static uint8_t s_ff[RX_BUF_SIZE];

static struct pbl_bt_addr s_addr;
static bool s_addr_valid;
// The opcode of the host's last command, which its Command Status or Complete answers
static uint16_t s_last_op;

// Masked until the thread has seen to it, so that a line that toggles cannot hold the CPU
static void prv_irq_handler(void *arg) {
  (void)pbl_intc_disable(&s_irq);
  pbl_sem_give(&s_irq_rose);
  pbl_sem_give(&s_wake);
}

static void prv_irq_arm(void) {
  (void)pbl_intc_enable(&s_irq);
}

static bool prv_irq_high(void) {
  return pbl_gpio_get(&NRG->irq) > 0;
}

static bool prv_wait_irq_low(void) {
  for (uint32_t us = 0U; us < IRQ_LOW_SPIN_US; us += IRQ_POLL_US) {
    if (!prv_irq_high()) {
      return true;
    }
    pbl_busy_wait_us(IRQ_POLL_US);
  }

  for (uint32_t ms = 0U; ms < IRQ_LOW_TIMEOUT_MS; ms++) {
    pbl_sleep_ms(1U);
    if (!prv_irq_high()) {
      return true;
    }
  }

  return false;
}

static int prv_xfer(const void *tx, void *rx, uint32_t len) {
  struct pbl_spi_xfer xfer = {.tx_buf = tx, .rx_buf = rx, .len = len};

  return pbl_spi_transfer(&NRG->spi, &xfer, 1U);
}

// One read, while IRQ is high: the bytes read, or a negative errno
static int prv_read(uint8_t *buf, uint32_t size) {
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
  len = PBL_MIN(len, size);
  if (len > 0U) {
    ret = prv_xfer(s_ff, buf, len);
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

// One write of a whole packet, retried while the chip has no room or is not ready. Data the chip
// has meanwhile goes to rx_cb.
static int prv_write(const uint8_t *buf, uint32_t len, void (*rx_cb)(void)) {
  static const uint8_t s_hdr[HDR_LEN] = {HDR_WRITE, 0U, 0U, 0U, 0U};
  uint64_t start = pbl_uptime_ms();
  uint8_t hdr[HDR_LEN];

  while (pbl_uptime_ms() - start < WRITE_TIMEOUT_MS) {
    bool ready = false;
    bool sent = false;
    int ret;

    if (prv_irq_high()) {
      rx_cb();
      continue;
    }

    (void)pbl_sem_take(&s_irq_rose, PBL_NO_WAIT);
    prv_irq_arm();
    ret = pbl_spi_hold(&NRG->spi);
    if (ret != 0) {
      return ret;
    }

    ready = prv_irq_high() ||
            (pbl_sem_take(&s_irq_rose, PBL_MSEC(IRQ_HIGH_TIMEOUT_MS)) == 0 && prv_irq_high());
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

// --- Start-up, before the host is given the chip ---------------------------------------------

// Packets read during the start-up, a whole H4 one at a time
static uint8_t s_init_pkt[RX_BUF_SIZE];
static uint32_t s_init_len;

static void prv_init_rx(void) {
  int n = prv_read(s_init_pkt, sizeof(s_init_pkt));

  s_init_len = n > 0 ? (uint32_t)n : 0U;
}

static bool prv_wait_irq_high(uint32_t timeout_ms) {
  uint64_t start = pbl_uptime_ms();

  while (!prv_irq_high()) {
    uint64_t waited = pbl_uptime_ms() - start;

    if (waited >= timeout_ms) {
      return false;
    }
    prv_irq_arm();
    if (prv_irq_high()) {
      break;
    }
    (void)pbl_sem_take(&s_irq_rose, PBL_MSEC((uint32_t)(timeout_ms - waited)));
  }

  return true;
}

// A command, and its Command Complete: its status, or a negative errno. The rest of its return
// parameters go to ret.
static int prv_init_cmd(uint16_t op, const uint8_t *par, uint8_t par_len, uint8_t *ret,
                        size_t *ret_len) {
  uint64_t start;
  int rc;

  s_tx_buf[0] = HCI_H4_CMD;
  s_tx_buf[1] = (uint8_t)op;
  s_tx_buf[2] = (uint8_t)(op >> 8);
  s_tx_buf[3] = par_len;
  memcpy(&s_tx_buf[4], par, par_len);

  s_init_len = 0U;
  rc = prv_write(s_tx_buf, 4U + par_len, prv_init_rx);
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
    if (n >= 7U && p[0] == HCI_H4_EVT && p[1] == BLE_HCI_EVCODE_COMMAND_COMPLETE &&
        (uint16_t)(p[4] | (p[5] << 8)) == op) {
      size_t len = PBL_MIN((size_t)p[2] - 3U, n - 6U);

      if (ret != nullptr) {
        *ret_len = PBL_MIN(*ret_len, len - 1U);
        memcpy(ret, &p[7], *ret_len);
      }
      return p[6];
    }
    if (n >= 7U && p[0] == HCI_H4_EVT && p[1] == BLE_HCI_EVCODE_COMMAND_STATUS &&
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
  if (s_init_len >= 6U && s_init_pkt[0] == HCI_H4_EVT && s_init_pkt[1] == BLE_HCI_EVCODE_VS &&
      (s_init_pkt[3] | (s_init_pkt[4] << 8)) == VS_BLUE_INITIALIZED) {
    return s_init_pkt[5];
  }

  return -EIO;
}

// Held in reset with the chip select undriven, then started: started normally or not
static int prv_restart(uint32_t hold_ms) {
  const struct pbl_bt_hci_bluenrg2 *nrg = NRG;
  const struct pbl_gpio *cs = &nrg->spi.bus->cs_gpios[nrg->spi.cs];
  int ret;

  (void)pbl_gpio_set(&nrg->reset, true);
  if (pbl_gpio_is_connected(cs)) {
    (void)pbl_gpio_configure(cs, PBL_GPIO_INPUT);
  }
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

// The 48 bits the chip keeps at config 0x80, least significant octet first, made a static random
// address by their two top bits, as the Core specification has it (Vol 6, Part B, 1.3.2.1): the
// chip's own stack does not run in link-layer-only mode to do it.
static void prv_read_identity(void) {
  static const uint8_t s_off = CONFIG_RANDOM_ADDR;
  uint8_t rsp[1U + sizeof(s_addr.octets)];
  size_t len = sizeof(rsp);
  uint8_t ones = 0xffU;
  uint8_t any = 0U;
  int rc;

  rc = prv_init_cmd(OP_ACI_READ_CONFIG, &s_off, 1U, rsp, &len);
  s_addr_valid = rc == 0 && len == sizeof(rsp) && rsp[0] == sizeof(s_addr.octets);
  if (s_addr_valid) {
    memcpy(s_addr.octets, &rsp[1], sizeof(s_addr.octets));
    s_addr.octets[5] |= 0xc0U;
    for (size_t i = 0U; i < sizeof(s_addr.octets); i++) {
      uint8_t random = i == 5U ? (s_addr.octets[i] & 0x3fU) : s_addr.octets[i];

      any |= random;
      ones &= i == 5U ? (random | 0xc0U) : random;
    }
    // Its random part all zeros or all ones is no address
    s_addr_valid = any != 0U && ones != 0xffU;
  }

  if (s_addr_valid) {
    PBL_LOG_DBG("Identity address %02x:%02x:%02x:%02x:%02x:%02x", s_addr.octets[5],
                s_addr.octets[4], s_addr.octets[3], s_addr.octets[2], s_addr.octets[1],
                s_addr.octets[0]);
  } else {
    PBL_LOG_WRN("No address in the chip's configuration (%d)", rc);
  }
}

static int prv_setup(void) {
  static const uint8_t s_ll_only[] = {CONFIG_LL_ONLY, 1U, 1U};
  uint8_t rsp[16];
  size_t len = sizeof(rsp);
  int rc;

  // First after the start, as ST's drivers do
  rc = prv_init_cmd(OP_ACI_WRITE_CONFIG, s_ll_only, sizeof(s_ll_only), nullptr, nullptr);
  if (rc != 0) {
    PBL_LOG_ERR("Link-layer-only mode refused (%d)", rc);
    return -EIO;
  }

  // In link-layer-only mode too, ACL data only flows once the chip's stack is initialised, as
  // STM32duinoBLE does it; the GATT and GAP of the chip go unused
  rc = prv_init_cmd(OP_ACI_GATT_INIT, nullptr, 0U, nullptr, nullptr);
  if (rc == 0) {
    static const uint8_t s_gap[] = {0x0fU, 0U, 0U};

    len = sizeof(rsp);
    rc = prv_init_cmd(OP_ACI_GAP_INIT, s_gap, sizeof(s_gap), rsp, &len);
  }
  if (rc != 0) {
    PBL_LOG_ERR("Stack init refused (%d)", rc);
    return -EIO;
  }

  len = sizeof(rsp);
  rc = prv_init_cmd(OP_ACI_FW_DETAILS, nullptr, 0U, rsp, &len);
  if (rc == 0 && len >= 14U) {
    PBL_LOG_DBG("BlueNRG-2 up: DTM %u.%u.%u, stack %u.%u.%u", rsp[0], rsp[1], rsp[2], rsp[6],
                rsp[7], rsp[8]);
  }

  prv_read_identity();

  return 0;
}

// Started normally and set up, trying again from a long reset
static void prv_bring_up(bool first) {
  uint32_t tries = 0U;

  while (true) {
    uint64_t up_ms = pbl_uptime_ms();
    // At boot the chip has been in reset since its device came up
    uint32_t hold = (first && tries == 0U && up_ms < RESET_HOLD_MS)
                        ? (uint32_t)(RESET_HOLD_MS - up_ms)
                        : (first && tries == 0U ? RESET_PULSE_MS : RESET_HOLD_MS);
    int reason = prv_restart(hold);

    tries++;
    if (reason == START_REASON_NORMAL && prv_setup() == 0) {
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

// --- To and from the host ----------------------------------------------------------------------

static int prv_hci_frame_cb(uint8_t pkt_type, void *data) {
  struct ble_hci_ev *ev = data;

  switch (pkt_type) {
    case HCI_H4_EVT:
      if (ev->opcode == BLE_HCI_EVCODE_VS) {
        if (ev->length >= 2U && (ev->data[0] | (ev->data[1] << 8)) == VS_BLUE_INITIALIZED) {
          PBL_LOG_WRN("BlueNRG-2 restarted by itself (reason %u)",
                      ev->length >= 3U ? ev->data[2] : 0U);
          s_up = false;
        }
        ble_transport_free(data);
        return 0;
      }
      // The DTM answers some refusals, as Controller Busy for LE Read Remote Features just after a
      // connection, with the opcode's OGF bits clear; the host would take it for another command's
      // and reset
      if (ev->opcode == BLE_HCI_EVCODE_COMMAND_STATUS && ev->length >= 4U) {
        uint16_t op = (uint16_t)(ev->data[2] | (ev->data[3] << 8));

        if (op != 0U && op != s_last_op && op == (s_last_op & 0x03ffU)) {
          PBL_LOG_DBG("Command Status for %04x given as %04x", s_last_op, op);
          ev->data[2] = (uint8_t)s_last_op;
          ev->data[3] = (uint8_t)(s_last_op >> 8);
        }
      }
      if (ev->opcode == BLE_HCI_EVCODE_COMMAND_COMPLETE && ev->length >= 10U) {
        struct ble_hci_ev_command_complete *cc = (void *)ev->data;

        // The DTM's placeholder 02:80:e1:00:00:00 is no address the host should use
        if (le16toh(cc->opcode) == OP_HCI_READ_BD_ADDR && cc->status == 0U) {
          memset(&cc->return_params[0], 0, 6U);
        }
      }
      return ble_transport_to_hs_evt(data);
    case HCI_H4_ACL:
      return ble_transport_to_hs_acl(data);
    default:
      break;
  }

  PBL_LOG_ERR("Unexpected H4 packet type %u", pkt_type);
  return -1;
}

static void prv_rx(void) {
  int n = prv_read(s_rx_buf, sizeof(s_rx_buf));

  for (int done = 0; done < n;) {
    int consumed = hci_h4_sm_rx(&s_hci_h4sm, &s_rx_buf[done], (uint16_t)(n - done));

    if (consumed <= 0) {
      PBL_LOG_ERR("hci_h4_sm_rx returned %d", consumed);
      hci_h4_sm_init(&s_hci_h4sm, &nimble_hci__allocs_from_ll, prv_hci_frame_cb);
      break;
    }
    done += consumed;
  }
}

static void prv_send_to_hs(uint8_t evcode, const uint8_t *par, uint8_t len) {
  struct ble_hci_ev *ev = ble_transport_alloc_evt(0);

  if (ev == nullptr) {
    PBL_LOG_ERR("No event buffer for event %02x", evcode);
    return;
  }

  ev->opcode = evcode;
  ev->length = len;
  memcpy(ev->data, par, len);
  (void)ble_transport_to_hs_evt(ev);
}

static void prv_answer_reset(void) {
  static const uint8_t s_cc[] = {1U, (uint8_t)OP_HCI_RESET, (uint8_t)(OP_HCI_RESET >> 8), 0U};

  prv_send_to_hs(BLE_HCI_EVCODE_COMMAND_COMPLETE, s_cc, sizeof(s_cc));
}

static void prv_free(const struct tx_entry *e) {
  if (e->type == HCI_H4_CMD) {
    ble_transport_free(e->buf);
  } else {
    os_mbuf_free_chain(e->buf);
  }
}

static bool prv_dequeue(struct tx_entry *e) {
  bool ok;

  pbl_mutex_lock(&s_queue_lock, PBL_FOREVER);
  ok = s_queue_len > 0U;
  if (ok) {
    *e = s_queue[s_queue_head];
    s_queue_head = (s_queue_head + 1U) % TX_QUEUE_LEN;
    s_queue_len--;
  }
  pbl_mutex_unlock(&s_queue_lock);

  return ok;
}

static int prv_enqueue(uint8_t type, void *buf) {
  int ret = 0;

  pbl_mutex_lock(&s_queue_lock, PBL_FOREVER);
  if (s_queue_len < TX_QUEUE_LEN) {
    s_queue[(s_queue_head + s_queue_len) % TX_QUEUE_LEN] = (struct tx_entry){type, buf};
    s_queue_len++;
  } else {
    ret = -ENOMEM;
  }
  pbl_mutex_unlock(&s_queue_lock);

  if (ret == 0) {
    pbl_sem_give(&s_wake);
  }

  return ret;
}

static void prv_send(const struct tx_entry *e) {
  uint32_t len = 1U;
  int ret;

  s_tx_buf[0] = e->type;
  if (e->type == HCI_H4_CMD) {
    const struct ble_hci_cmd *cmd = e->buf;

    len += sizeof(*cmd) + cmd->length;
    memcpy(&s_tx_buf[1], cmd, len - 1U);
    s_last_op = le16toh(cmd->opcode);
  } else {
    uint32_t pktlen = OS_MBUF_PKTLEN((struct os_mbuf *)e->buf);

    if (pktlen > sizeof(s_tx_buf) - 1U) {
      PBL_LOG_ERR("ACL packet of %lu bytes dropped", (unsigned long)pktlen);
      prv_free(e);
      return;
    }
    (void)os_mbuf_copydata(e->buf, 0, (int)pktlen, &s_tx_buf[1]);
    len += pktlen;
  }
  prv_free(e);

  ret = prv_write(s_tx_buf, len, prv_rx);
  if (ret != 0) {
    PBL_LOG_ERR("Write of %lu bytes failed (%d)", (unsigned long)len, ret);
  }
}

static void prv_flush(void) {
  struct tx_entry e;

  while (prv_dequeue(&e)) {
    prv_free(&e);
  }
}

static void prv_hci_task_main(void *unused) {
  const uint8_t hw_error = 0U;
  bool first = true;

  while (true) {
    prv_bring_up(first);
    if (!first) {
      // The host's state of the controller is gone with its restart
      prv_flush();
      prv_send_to_hs(BLE_HCI_EVCODE_HW_ERROR, &hw_error, 1U);
    }
    first = false;
    s_up = true;

    while (s_up) {
      struct tx_entry e;

      if (s_reset_pending) {
        s_reset_pending = false;
        prv_answer_reset();
      } else if (prv_irq_high()) {
        prv_rx();
      } else if (prv_dequeue(&e)) {
        prv_send(&e);
      } else {
        prv_irq_arm();
        if (!prv_irq_high()) {
          (void)pbl_sem_take(&s_wake, PBL_FOREVER);
        }
      }
    }
  }
}

void ble_transport_ll_init(void) {
  int ret;

  nimble_hci__alloc_init();
  hci_h4_sm_init(&s_hci_h4sm, &nimble_hci__allocs_from_ll, prv_hci_frame_cb);
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

  struct pbl_thread_attr attr = {
    .name = "NimbleHCI",
    .entry = prv_hci_task_main,
    .prio = CONFIG_NIMBLE_HCI_THREAD_PRIO,
    .privileged = true,
    .stack = s_hci_task_stack,
    .stack_size = sizeof(s_hci_task_stack),
  };

  PBL_ASSERTN(pbl_thread_create(&s_hci_thread, &attr) == 0);
}

int ble_transport_to_ll_cmd_impl(void *buf) {
  struct ble_hci_cmd *cmd = buf;

  // Answered here: it would start the chip's updater
  if (le16toh(cmd->opcode) == OP_HCI_RESET) {
    ble_transport_free(buf);
    s_reset_pending = true;
    pbl_sem_give(&s_wake);
    return 0;
  }

  if (prv_enqueue(HCI_H4_CMD, buf) != 0) {
    ble_transport_free(buf);
    return BLE_ERR_MEM_CAPACITY;
  }

  return 0;
}

int ble_transport_to_ll_acl_impl(struct os_mbuf *om) {
  if (prv_enqueue(HCI_H4_ACL, om) != 0) {
    os_mbuf_free_chain(om);
    return BLE_ERR_MEM_CAPACITY;
  }

  return 0;
}

int ble_transport_to_ll_iso_impl(struct os_mbuf *om) {
  os_mbuf_free_chain(om);

  return BLE_ERR_UNSUPPORTED;
}

struct pbl_thread *nimble_port_pebble_hci_thread(void) {
  return &s_hci_thread;
}

#ifdef CONFIG_NIMBLE_ID_ADDR_BLUENRG2
// The module's static random address, read at each start: stock UNA uses it as the watch's
int pbl_bt_id_addr_get(struct pbl_bt_addr *addr, enum pbl_bt_id_addr_type *type) {
  if (!s_addr_valid) {
    return -ENOENT;
  }

  *addr = s_addr;
  *type = PBL_BT_ID_ADDR_RANDOM_STATIC;

  return 0;
}
#endif
