/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The link to the BlueNRG-2 with its stack on: one thread does every SPI transaction. Commands
 * are sent one at a time, their callers waiting for the answer; every other event goes to the
 * host's queue, never blocking this thread, so that a command's answer always gets through.
 */

#include "aci_priv.h"
#include "bluenrg2_spi.h"

#include <errno.h>

#include <pbl/kernel/assert.h>
#include <pbl/kernel/malloc.h>
#include <pbl/kernel/mutex.h>
#include <pbl/kernel/sem.h>
#include <pbl/kernel/thread.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/util/math.h>

#include <nimble/nimble_npl.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_pebble.h>

PBL_LOG_MODULE_DECLARE(nimble_aci, CONFIG_NIMBLE_ACI_BLUENRG2_LOG_LEVEL);

#define CMD_TIMEOUT_MS 2000U
// A command answered later than this is logged
#define SLOW_CMD_MS     100U
#define STOP_TIMEOUT_MS 3000U

#define RX_BUF_SIZE 512U
#define EVT_MAX     (2U + ACI_MAX_PARAMS)
#define POOL_SIZE   16U

struct evt {
  struct evt *next;
  bool pooled;
  uint16_t len;
  uint8_t data[EVT_MAX];
};

static struct pbl_thread s_thread;
PBL_THREAD_STACK_DEFINE(s_stack, CONFIG_NIMBLE_HCI_THREAD_STACK_SIZE);

// The command, one at a time
static PBL_MUTEX_DEFINE(s_cmd_lock);
// Its state, between its caller and the thread
static PBL_MUTEX_DEFINE(s_state_lock);
static PBL_SEM_DEFINE(s_cmd_done, 0, 1);
static struct {
  bool pending;
  bool sent;
  uint16_t op;
  uint16_t len;
  uint8_t pkt[4U + ACI_MAX_PARAMS];
  int status;
  uint8_t *rsp;
  size_t rsp_len;
  // When it was asked, and written to the chip
  uint64_t asked_ms;
  uint64_t written_ms;
} s_cmd;

// Started or stopped by the host
static PBL_SEM_DEFINE(s_ctl, 0, 1);
static PBL_SEM_DEFINE(s_stopped, 0, 1);
static volatile bool s_want_up;
static volatile bool s_up;
static bool s_started_once;

static uint8_t s_addr[6];
static bool s_addr_valid;

// Events to the host
static struct evt s_pool[POOL_SIZE];
static struct evt *s_free;
static struct evt *s_head;
static struct evt *s_tail;
static struct ble_npl_event s_rx_ev;
static uint32_t s_overflows;

static uint8_t s_rx_buf[RX_BUF_SIZE];
// A packet split across reads
static uint8_t s_asm[3U + ACI_MAX_PARAMS + 2U];
static uint32_t s_asm_len;

// --- To the host -------------------------------------------------------------------------------

static void prv_post(const uint8_t *data, uint16_t len) {
  struct evt *e;

  pbl_mutex_lock(&s_state_lock, PBL_FOREVER);
  e = s_free;
  if (e != nullptr) {
    s_free = e->next;
  }
  pbl_mutex_unlock(&s_state_lock);

  if (e == nullptr) {
    e = pbl_malloc(sizeof(*e));
    if (e == nullptr) {
      PBL_LOG_ERR("Event %02x dropped: no memory", data[0]);
      return;
    }
    e->pooled = false;
    s_overflows++;
  } else {
    e->pooled = true;
  }

  e->next = nullptr;
  e->len = PBL_MIN(len, (uint16_t)EVT_MAX);
  memcpy(e->data, data, e->len);

  pbl_mutex_lock(&s_state_lock, PBL_FOREVER);
  if (s_tail != nullptr) {
    s_tail->next = e;
  } else {
    s_head = e;
  }
  s_tail = e;
  pbl_mutex_unlock(&s_state_lock);

  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_rx_ev);
}

static void prv_post_link(bool up) {
  const uint8_t evt[] = {ACI_EVT_LINK, 1U, up ? 1U : 0U};

  prv_post(evt, sizeof(evt));
}

// On the host's thread
static void prv_rx_ev_fn(struct ble_npl_event *ev) {
  while (true) {
    struct evt *e;

    pbl_mutex_lock(&s_state_lock, PBL_FOREVER);
    e = s_head;
    if (e != nullptr) {
      s_head = e->next;
      if (s_head == nullptr) {
        s_tail = nullptr;
      }
    }
    pbl_mutex_unlock(&s_state_lock);

    if (e == nullptr) {
      return;
    }

    aci_hs_rx(e->data, e->len);

    if (e->pooled) {
      pbl_mutex_lock(&s_state_lock, PBL_FOREVER);
      e->next = s_free;
      s_free = e;
      pbl_mutex_unlock(&s_state_lock);
    } else {
      pbl_free(e);
    }
  }
}

// --- From the chip -----------------------------------------------------------------------------

static void prv_complete(int status, const uint8_t *ret, size_t ret_len) {
  pbl_mutex_lock(&s_state_lock, PBL_FOREVER);
  if (s_cmd.pending && s_cmd.sent) {
    s_cmd.status = status;
    if (s_cmd.rsp != nullptr) {
      s_cmd.rsp_len = PBL_MIN(s_cmd.rsp_len, ret_len);
      memcpy(s_cmd.rsp, ret, s_cmd.rsp_len);
    } else {
      s_cmd.rsp_len = 0U;
    }
    s_cmd.pending = false;
    pbl_sem_give(&s_cmd_done);
  }
  pbl_mutex_unlock(&s_state_lock);
}

static bool prv_is_cmd_op(uint16_t op) {
  return op != 0U && op == s_cmd.op;
}

// A whole event: code, length, parameters
static void prv_rx_evt(const uint8_t *p, uint32_t n) {
  uint8_t code = p[0];
  const uint8_t *par = &p[2];
  uint32_t plen = n - 2U;

  if (code == ACI_EVT_CMD_COMPLETE && plen >= 4U) {
    if (prv_is_cmd_op(aci_get_le16(&par[1]))) {
      prv_complete(par[3], &par[4], plen - 4U);
    }
    return;
  }
  if (code == ACI_EVT_CMD_STATUS && plen >= 4U) {
    if (prv_is_cmd_op(aci_get_le16(&par[2]))) {
      prv_complete(par[0], nullptr, 0U);
    }
    return;
  }
  if (code == ACI_EVT_NUM_COMP_PKTS) {
    return;
  }
  if (code == ACI_EVT_VS && plen >= 2U && aci_get_le16(par) == ACI_VS_BLUE_INITIALIZED) {
    PBL_LOG_WRN("BlueNRG-2 restarted by itself (reason %u)", plen >= 3U ? par[2] : 0U);
    s_up = false;
    return;
  }

  prv_post(p, (uint16_t)n);
}

static void prv_rx(void) {
  int n = bnrg_read(s_rx_buf, sizeof(s_rx_buf));

  for (int i = 0; i < n; i++) {
    s_asm[s_asm_len++] = s_rx_buf[i];

    if (s_asm[0] != BNRG_PKT_EVT) {
      static uint32_t s_odd;

      if (s_odd++ < 10U) {
        PBL_LOG_WRN("Packet of type %02x dropped", s_asm[0]);
      }
      s_asm_len = 0U;
      // The rest of this read is the packet's
      return;
    }
    if (s_asm_len >= 3U && s_asm_len == 3U + s_asm[2]) {
      prv_rx_evt(&s_asm[1], s_asm_len - 1U);
      s_asm_len = 0U;
    }
  }
}

// --- The thread --------------------------------------------------------------------------------

static int prv_setup(void) {
  uint8_t rsp[16];
  size_t len = sizeof(rsp);
  struct pbl_bt_addr addr;
  int rc;

  rc = bnrg_init_cmd(ACI_OP_HAL_FW_DETAILS, nullptr, 0U, rsp, &len);
  if (rc != 0) {
    return -EIO;
  }
  if (len >= 14U) {
    PBL_LOG_DBG("BlueNRG-2 up: DTM %u.%u.%u, stack %u.%u.%u", rsp[0], rsp[1], rsp[2], rsp[6],
                rsp[7], rsp[8]);
  }

  s_addr_valid = bnrg_read_identity(&addr) == 0;
  if (s_addr_valid) {
    memcpy(s_addr, addr.octets, sizeof(s_addr));
  }

  return 0;
}

static void prv_fail_cmd(void) {
  pbl_mutex_lock(&s_state_lock, PBL_FOREVER);
  if (s_cmd.pending) {
    s_cmd.status = -ENOTCONN;
    s_cmd.pending = false;
    pbl_sem_give(&s_cmd_done);
  }
  pbl_mutex_unlock(&s_state_lock);
}

static void prv_send(void) {
  // Its own copy: a caller that gave up may be filling the next command meanwhile
  static uint8_t s_pkt[4U + ACI_MAX_PARAMS];
  uint16_t len = 0U;
  uint16_t op = 0U;
  int rc;

  pbl_mutex_lock(&s_state_lock, PBL_FOREVER);
  bool go = s_cmd.pending && !s_cmd.sent;
  if (go) {
    s_cmd.sent = true;
    len = s_cmd.len;
    op = s_cmd.op;
    memcpy(s_pkt, s_cmd.pkt, len);
  }
  pbl_mutex_unlock(&s_state_lock);

  if (!go) {
    return;
  }

  rc = bnrg_write(s_pkt, len, prv_rx);
  pbl_mutex_lock(&s_state_lock, PBL_FOREVER);
  if (s_cmd.op == op) {
    s_cmd.written_ms = pbl_uptime_ms();
  }
  if (rc != 0) {
    PBL_LOG_ERR("Command %04x not written (%d)", op, rc);
    if (s_cmd.pending && s_cmd.op == op) {
      s_cmd.status = rc;
      s_cmd.pending = false;
      pbl_sem_give(&s_cmd_done);
    }
  }
  pbl_mutex_unlock(&s_state_lock);
}

static bool prv_cmd_to_send(void) {
  return s_cmd.pending && !s_cmd.sent;
}

static void prv_main(void *arg) {
  while (true) {
    while (!s_want_up) {
      (void)pbl_sem_take(&s_ctl, PBL_FOREVER);
    }

    s_asm_len = 0U;
    bnrg_bring_up(!s_started_once, prv_setup);
    s_started_once = true;
    s_up = true;
    prv_post_link(true);

    while (s_up && s_want_up) {
      if (bnrg_irq_high()) {
        prv_rx();
      } else if (prv_cmd_to_send()) {
        prv_send();
      } else {
        bnrg_wait();
      }
    }

    s_up = false;
    prv_fail_cmd();
    if (!s_want_up) {
      bnrg_hold_reset();
      pbl_sem_give(&s_stopped);
    } else {
      prv_post_link(false);
    }
  }
}

// --- API ---------------------------------------------------------------------------------------

int aci_cmd(uint16_t op, const void *par, uint8_t par_len, void *rsp, size_t *rsp_len) {
  int rc;

  pbl_mutex_lock(&s_cmd_lock, PBL_FOREVER);

  pbl_mutex_lock(&s_state_lock, PBL_FOREVER);
  if (!s_up) {
    pbl_mutex_unlock(&s_state_lock);
    pbl_mutex_unlock(&s_cmd_lock);
    return -ENOTCONN;
  }
  s_cmd.op = op;
  s_cmd.pkt[0] = BNRG_PKT_CMD;
  (void)aci_put_le16(&s_cmd.pkt[1], op);
  s_cmd.pkt[3] = par_len;
  if (par_len > 0U) {
    memcpy(&s_cmd.pkt[4], par, par_len);
  }
  s_cmd.len = 4U + par_len;
  s_cmd.rsp = rsp;
  s_cmd.rsp_len = rsp != nullptr ? *rsp_len : 0U;
  s_cmd.status = -ETIMEDOUT;
  s_cmd.asked_ms = pbl_uptime_ms();
  s_cmd.written_ms = 0U;
  s_cmd.sent = false;
  s_cmd.pending = true;
  (void)pbl_sem_take(&s_cmd_done, PBL_NO_WAIT);
  pbl_mutex_unlock(&s_state_lock);

  bnrg_kick();
  (void)pbl_sem_take(&s_cmd_done, PBL_MSEC(CMD_TIMEOUT_MS));

  pbl_mutex_lock(&s_state_lock, PBL_FOREVER);
  uint64_t now = pbl_uptime_ms();
  if (s_cmd.pending) {
    PBL_LOG_ERR("Command %04x not answered, written after %lu ms", op,
                s_cmd.written_ms != 0U ? (unsigned long)(s_cmd.written_ms - s_cmd.asked_ms) : 0UL);
    s_cmd.pending = false;
  } else if (now - s_cmd.asked_ms > SLOW_CMD_MS) {
    PBL_LOG_WRN("Command %04x answered after %lu ms, written after %lu ms", op,
                (unsigned long)(now - s_cmd.asked_ms),
                s_cmd.written_ms != 0U ? (unsigned long)(s_cmd.written_ms - s_cmd.asked_ms) : 0UL);
  }
  rc = s_cmd.status;
  if (rsp_len != nullptr) {
    *rsp_len = s_cmd.rsp_len;
  }
  pbl_mutex_unlock(&s_state_lock);

  pbl_mutex_unlock(&s_cmd_lock);

  if (rc != 0) {
    PBL_LOG_DBG("Command %04x: status %d", op, rc);
  }

  return rc;
}

void aci_link_start(void) {
  s_want_up = true;
  pbl_sem_give(&s_ctl);
  bnrg_kick();
}

void aci_link_stop(void) {
  (void)pbl_sem_take(&s_stopped, PBL_NO_WAIT);
  if (!s_want_up) {
    return;
  }

  s_want_up = false;
  bnrg_kick();
  if (pbl_sem_take(&s_stopped, PBL_MSEC(STOP_TIMEOUT_MS)) != 0) {
    PBL_LOG_ERR("BlueNRG-2 not stopped");
  }
}

int aci_link_identity(uint8_t addr[6]) {
  if (!s_addr_valid) {
    return -ENOENT;
  }

  memcpy(addr, s_addr, sizeof(s_addr));

  return 0;
}

void aci_link_init(void) {
  for (size_t i = 0U; i < POOL_SIZE; i++) {
    s_pool[i].next = s_free;
    s_free = &s_pool[i];
  }
  ble_npl_event_init(&s_rx_ev, prv_rx_ev_fn, nullptr);

  bnrg_spi_init();

  struct pbl_thread_attr attr = {
    .name = "NimbleHCI",
    .entry = prv_main,
    .prio = CONFIG_NIMBLE_HCI_THREAD_PRIO,
    .privileged = true,
    .stack = s_stack,
    .stack_size = sizeof(s_stack),
  };

  PBL_ASSERTN(pbl_thread_create(&s_thread, &attr) == 0);
}

struct pbl_thread *nimble_port_pebble_hci_thread(void) {
  return &s_thread;
}

#ifdef CONFIG_NIMBLE_ID_ADDR_BLUENRG2
// The module's static random address, read at each start: stock UNA uses it as the watch's
int pbl_bt_id_addr_get(struct pbl_bt_addr *addr, enum pbl_bt_id_addr_type *type) {
  if (!s_addr_valid) {
    return -ENOENT;
  }

  memcpy(addr->octets, s_addr, sizeof(addr->octets));
  *type = PBL_BT_ID_ADDR_RANDOM_STATIC;

  return 0;
}
#endif
