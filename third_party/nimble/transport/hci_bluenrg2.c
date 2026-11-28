/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * HCI to an ST BlueNRG-2 running ST's DTM firmware (SPI variant), in link-layer-only mode, over
 * its SPI protocol (bluenrg2_spi.c). One thread does every transaction: the host's packets are
 * queued to it.
 *
 * The chip's quirks, measured on the UNA Watch (docs/boards/unawatch/hardware.md, 2.5.1):
 * HCI_Reset with the SPI lines driven starts its updater, where it answers no command: the host's
 * is answered here and never sent. Link-layer-only mode, which should hand ACL data to the host,
 * is set after each start; it is lost at the next. The chip has no public address: it reports a
 * placeholder, hidden from the host, and the module's static random address is the identity
 * address.
 */

#include "bluenrg2_spi.h"
#include "hci_alloc.h"

#include <errno.h>
#include <string.h>

#include <pbl/bluetooth/id_addr.h>
#include <pbl/kernel/assert.h>
#include <pbl/kernel/mutex.h>
#include <pbl/kernel/thread.h>
#include <pbl/logging/logging.h>

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

#define OP_HCI_RESET        0x0c03U
#define OP_HCI_READ_BD_ADDR 0x1009U
#define OP_ACI_WRITE_CONFIG 0xfc0cU
#define OP_ACI_FW_DETAILS   0xfc01U
#define OP_ACI_GAP_INIT     0xfc8aU
#define OP_ACI_GATT_INIT    0xfd01U

#define CONFIG_LL_ONLY 0x2cU

#define RX_BUF_SIZE 512U
// An ACL packet of 251 bytes with its headers, or a command of 255
#define TX_BUF_SIZE  (1U + 4U + 255U)
#define TX_QUEUE_LEN 16U

struct tx_entry {
  uint8_t type;
  void *buf;
};

static struct pbl_thread s_hci_thread;
PBL_THREAD_STACK_DEFINE(s_hci_task_stack, CONFIG_NIMBLE_HCI_THREAD_STACK_SIZE);
static PBL_MUTEX_DEFINE(s_queue_lock);
static struct hci_h4_sm s_hci_h4sm;

static struct tx_entry s_queue[TX_QUEUE_LEN];
static uint32_t s_queue_head;
static uint32_t s_queue_len;
// The host asked for a reset, answered once the chip is up
static volatile bool s_reset_pending;
// The chip has started and is set up; the host's packets go to it
static bool s_up;

static uint8_t s_rx_buf[RX_BUF_SIZE];
static uint8_t s_tx_buf[TX_BUF_SIZE];

static struct pbl_bt_addr s_addr;
static bool s_addr_valid;
// The opcode of the host's last command, which its Command Status or Complete answers
static uint16_t s_last_op;

// --- Start-up, before the host is given the chip ---------------------------------------------

static int prv_setup(void) {
  static const uint8_t s_ll_only[] = {CONFIG_LL_ONLY, 1U, 1U};
  uint8_t rsp[16];
  size_t len = sizeof(rsp);
  int rc;

  // First after the start, as ST's drivers do
  rc = bnrg_init_cmd(OP_ACI_WRITE_CONFIG, s_ll_only, sizeof(s_ll_only), nullptr, nullptr);
  if (rc != 0) {
    PBL_LOG_ERR("Link-layer-only mode refused (%d)", rc);
    return -EIO;
  }

  // In link-layer-only mode too, ACL data only flows once the chip's stack is initialised, as
  // STM32duinoBLE does it; the GATT and GAP of the chip go unused
  rc = bnrg_init_cmd(OP_ACI_GATT_INIT, nullptr, 0U, nullptr, nullptr);
  if (rc == 0) {
    static const uint8_t s_gap[] = {0x0fU, 0U, 0U};

    len = sizeof(rsp);
    rc = bnrg_init_cmd(OP_ACI_GAP_INIT, s_gap, sizeof(s_gap), rsp, &len);
  }
  if (rc != 0) {
    PBL_LOG_ERR("Stack init refused (%d)", rc);
    return -EIO;
  }

  len = sizeof(rsp);
  rc = bnrg_init_cmd(OP_ACI_FW_DETAILS, nullptr, 0U, rsp, &len);
  if (rc == 0 && len >= 14U) {
    PBL_LOG_DBG("BlueNRG-2 up: DTM %u.%u.%u, stack %u.%u.%u", rsp[0], rsp[1], rsp[2], rsp[6],
                rsp[7], rsp[8]);
  }

  s_addr_valid = bnrg_read_identity(&s_addr) == 0;

  return 0;
}

// --- To and from the host ----------------------------------------------------------------------

static int prv_hci_frame_cb(uint8_t pkt_type, void *data) {
  struct ble_hci_ev *ev = data;

  switch (pkt_type) {
    case HCI_H4_EVT:
      if (ev->opcode == BLE_HCI_EVCODE_VS) {
        if (ev->length >= 2U && (ev->data[0] | (ev->data[1] << 8)) == BNRG_VS_BLUE_INITIALIZED) {
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
  int n = bnrg_read(s_rx_buf, sizeof(s_rx_buf));

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
    bnrg_kick();
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

  ret = bnrg_write(s_tx_buf, len, prv_rx);
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
    bnrg_bring_up(first, prv_setup);
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
      } else if (bnrg_irq_high()) {
        prv_rx();
      } else if (prv_dequeue(&e)) {
        prv_send(&e);
      } else {
        bnrg_wait();
      }
    }
  }
}

void ble_transport_ll_init(void) {
  nimble_hci__alloc_init();
  hci_h4_sm_init(&s_hci_h4sm, &nimble_hci__allocs_from_ll, prv_hci_frame_cb);
  bnrg_spi_init();

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
    bnrg_kick();
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
