/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The host: its start, stop and reset, its identity, and the dispatch of the chip's events. It
 * runs on NimBLE's host thread, the default event queue's, as NimBLE's own host does. The chip is
 * held in reset while the host is stopped and restarted at each start, which gives it a fresh
 * GATT database for the services registered since.
 */

#include "aci_priv.h"

#include <errno.h>

#include <pbl/drivers/rng.h>
#include <pbl/kernel/mutex.h>
#include <pbl/logging/logging.h>

#include <host/ble_hs_stop.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/transport.h>
#include <nimble/transport_impl.h>

PBL_LOG_MODULE_DEFINE(nimble_aci, CONFIG_NIMBLE_ACI_BLUENRG2_LOG_LEVEL);

enum state {
  STATE_STOPPED,
  // The chip is starting, or being set up
  STATE_STARTING,
  STATE_SYNCED,
};

static PBL_MUTEX_DEFINE(s_lock);
static PBL_MUTEX_DEFINE(s_store_lock);
static enum state s_state;
static struct ble_npl_event s_start_ev;
static struct ble_npl_event s_stop_ev;
static struct ble_npl_event s_reset_ev;
static int s_reset_reason;
static struct ble_hs_stop_listener *s_stop_listener;

static uint8_t s_rnd_addr[6];
static bool s_rnd_addr_set;

void aci_lock(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
}

void aci_unlock(void) {
  pbl_mutex_unlock(&s_lock);
}

// ble_store.c's
void ble_hs_lock(void) {
  pbl_mutex_lock(&s_store_lock, PBL_FOREVER);
}

void ble_hs_unlock(void) {
  pbl_mutex_unlock(&s_store_lock);
}

int ble_hs_locked_by_cur_task(void) {
  return 0;
}

// The privacy of NimBLE's host, which the chip's has instead
int ble_hs_pvcy_add_entry(const uint8_t *addr, uint8_t addr_type, const uint8_t *irk) {
  return 0;
}

void ble_hs_log_flat_buf(const void *data, int len) {
}

int ble_hs_hci_rand(void *dst, int len) {
  uint8_t *p = dst;

  while (len > 0) {
    uint32_t word;
    int n = len < 4 ? len : 4;

    if (rng_rand(&word) != 0) {
      return BLE_HS_EOS;
    }
    memcpy(p, &word, (size_t)n);
    p += n;
    len -= n;
  }

  return 0;
}

int aci_hs_err(int status) {
  switch (status) {
    case 0:
      return 0;
    case -ENOTCONN:
      return BLE_HS_ENOTSYNCED;
    case -ETIMEDOUT:
      return BLE_HS_ETIMEOUT;
    case ACI_STATUS_INSUFFICIENT_RESOURCES:
      return BLE_HS_ENOMEM;
    case ACI_STATUS_BUSY:
      return BLE_HS_EBUSY;
    default:
      break;
  }

  if (status < 0) {
    return BLE_HS_EOS;
  }

  return BLE_HS_HCI_ERR(status);
}

// --- Set-up ------------------------------------------------------------------------------------

static int prv_cmd(uint16_t op, const void *par, uint8_t len) {
  int rc = aci_cmd(op, par, len, nullptr, nullptr);

  if (rc != 0) {
    PBL_LOG_ERR("Set-up command %04x failed (%d)", op, rc);
  }

  return rc;
}

static int prv_setup(void) {
  // The HCI event masks are left as they are: the chip's stack takes some of those events itself
  static const uint8_t s_gatt_mask[4] = {0xff, 0xff, 0x07, 0x00};
  static const uint8_t s_gap_mask[2] = {0xff, 0xff};
  int rc;

  rc = prv_cmd(ACI_OP_GATT_INIT, nullptr, 0U);
  if (rc == 0) {
    rc = aci_gap_setup();
  }
  if (rc == 0) {
    rc = prv_cmd(ACI_OP_GATT_SET_EVENT_MASK, s_gatt_mask, sizeof(s_gatt_mask));
  }
  if (rc == 0) {
    rc = prv_cmd(ACI_OP_GAP_SET_EVENT_MASK, s_gap_mask, sizeof(s_gap_mask));
  }
  if (rc == 0) {
    rc = aci_svc_setup();
  }
  if (rc == 0) {
    rc = aci_gatts_setup();
  }
  if (rc == 0 && s_rnd_addr_set) {
    rc = prv_cmd(ACI_OP_HCI_LE_SET_RANDOM_ADDR, s_rnd_addr, sizeof(s_rnd_addr));
  }

  return rc;
}

static void prv_synced(void) {
  int rc = prv_setup();

  if (rc != 0) {
    // Tried again from a restart of the chip
    PBL_LOG_ERR("BlueNRG-2 set-up failed (%d)", rc);
    aci_link_stop();
    aci_link_start();
    return;
  }

  aci_store_reconcile();

  aci_lock();
  s_state = STATE_SYNCED;
  aci_unlock();

  PBL_LOG_DBG("Host synchronised with the BlueNRG-2");
  if (ble_hs_cfg.sync_cb != nullptr) {
    ble_hs_cfg.sync_cb();
  }
}

// Every connection gone, as with a reset of NimBLE's host
static void prv_drop_all(int reason) {
  aci_gap_reset(reason);
}

// --- Events ------------------------------------------------------------------------------------

static void prv_rx_link(bool up) {
  if (up) {
    if (s_state == STATE_STARTING) {
      prv_synced();
    }
    return;
  }

  // Down by itself: what it had is gone; it is being started again
  if (s_state == STATE_SYNCED) {
    aci_lock();
    s_state = STATE_STARTING;
    aci_unlock();
    prv_drop_all(BLE_HS_ECONTROLLER);
    if (ble_hs_cfg.reset_cb != nullptr) {
      ble_hs_cfg.reset_cb(BLE_HS_ECONTROLLER);
    }
  }
}

void aci_hs_rx(const uint8_t *evt, size_t len) {
  uint8_t code;
  const uint8_t *p;
  size_t n;

  if (len < 2U) {
    return;
  }

  code = evt[0];
  p = &evt[2];
  n = len - 2U;

  if (code == ACI_EVT_LINK) {
    prv_rx_link(n >= 1U && p[0] != 0U);
    return;
  }

  if (s_state != STATE_SYNCED) {
    PBL_LOG_DBG("Event %02x before the set-up dropped", code);
    return;
  }

  switch (code) {
    case ACI_EVT_DISCONN_COMPLETE:
    case ACI_EVT_ENC_CHANGE:
    case ACI_EVT_ENC_KEY_REFRESH:
      aci_gap_rx_hci(code, p, n);
      break;
    case ACI_EVT_LE_META:
      if (n >= 1U) {
        aci_gap_rx_le(p[0], &p[1], n - 1U);
      }
      break;
    case ACI_EVT_HW_ERROR:
      PBL_LOG_ERR("BlueNRG-2 hardware error %u", n >= 1U ? p[0] : 0U);
      ble_hs_sched_reset(BLE_HS_ECONTROLLER);
      break;
    case ACI_EVT_VS:
      if (n >= 2U) {
        uint16_t vs = aci_get_le16(p);

        if (vs == ACI_VS_BLUE_EVENTS_LOST) {
          PBL_LOG_ERR("BlueNRG-2 lost events");
        } else if (vs == ACI_VS_BLUE_CRASH_INFO) {
          PBL_LOG_ERR("BlueNRG-2 crash information");
        } else if (!aci_gatts_rx_vs(vs, &p[2], n - 2U) && !aci_gattc_rx_vs(vs, &p[2], n - 2U) &&
                   !aci_gap_rx_vs(vs, &p[2], n - 2U)) {
          PBL_LOG_DBG("Vendor event %04x ignored", vs);
        }
      }
      break;
    default:
      PBL_LOG_DBG("Event %02x ignored", code);
      break;
  }
}

// --- Start, stop, reset ------------------------------------------------------------------------

static void prv_start_fn(struct ble_npl_event *ev) {
  if (s_state != STATE_STOPPED) {
    return;
  }

  aci_lock();
  s_state = STATE_STARTING;
  aci_unlock();
  aci_link_start();
}

static void prv_stop_fn(struct ble_npl_event *ev) {
  struct ble_hs_stop_listener *listener = s_stop_listener;

  s_stop_listener = nullptr;

  if (s_state == STATE_SYNCED) {
    prv_drop_all(BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL));
  }
  aci_link_stop();

  aci_lock();
  s_state = STATE_STOPPED;
  aci_unlock();

  if (listener != nullptr && listener->fn != nullptr) {
    listener->fn(0, listener->arg);
  }
}

static void prv_reset_fn(struct ble_npl_event *ev) {
  bool synced = s_state == STATE_SYNCED;

  if (s_state == STATE_STOPPED) {
    return;
  }

  if (synced) {
    prv_drop_all(s_reset_reason);
  }
  aci_link_stop();

  aci_lock();
  s_state = STATE_STARTING;
  aci_unlock();

  if (synced && ble_hs_cfg.reset_cb != nullptr) {
    ble_hs_cfg.reset_cb(s_reset_reason);
  }
  aci_link_start();
}

void ble_hs_sched_start(void) {
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_start_ev);
}

void ble_hs_sched_reset(int reason) {
  s_reset_reason = reason;
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_reset_ev);
}

int ble_hs_stop(struct ble_hs_stop_listener *listener, ble_hs_stop_fn *fn, void *arg) {
  aci_lock();
  bool stopped = s_state == STATE_STOPPED;
  aci_unlock();

  if (stopped) {
    return BLE_HS_EALREADY;
  }
  if (s_stop_listener != nullptr) {
    return BLE_HS_EBUSY;
  }

  listener->fn = fn;
  listener->arg = arg;
  s_stop_listener = listener;
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_stop_ev);

  return 0;
}

int ble_hs_synced(void) {
  return s_state == STATE_SYNCED;
}

int ble_hs_is_enabled(void) {
  return s_state != STATE_STOPPED;
}

// --- Identity ----------------------------------------------------------------------------------

int ble_hs_id_set_rnd(const uint8_t *rnd_addr) {
  int rc = 0;

  // A static random address: its top two bits set
  if ((rnd_addr[5] & 0xc0U) != 0xc0U) {
    return BLE_HS_EINVAL;
  }

  aci_lock();
  memcpy(s_rnd_addr, rnd_addr, sizeof(s_rnd_addr));
  s_rnd_addr_set = true;
  aci_unlock();

  if (s_state == STATE_SYNCED) {
    rc = aci_hs_err(aci_cmd(ACI_OP_HCI_LE_SET_RANDOM_ADDR, rnd_addr, 6U, nullptr, nullptr));
  }

  return rc;
}

int ble_hs_id_copy_addr(uint8_t id_addr_type, uint8_t *out_id_addr, int *out_is_nrpa) {
  if (out_is_nrpa != nullptr) {
    *out_is_nrpa = 0;
  }

  // The chip has no public address
  if (id_addr_type != BLE_ADDR_RANDOM) {
    return id_addr_type == BLE_ADDR_PUBLIC ? BLE_HS_ENOADDR : BLE_HS_EINVAL;
  }

  aci_lock();
  bool set = s_rnd_addr_set;
  if (set && out_id_addr != nullptr) {
    memcpy(out_id_addr, s_rnd_addr, sizeof(s_rnd_addr));
  }
  aci_unlock();

  return set ? 0 : BLE_HS_ENOADDR;
}

int ble_hs_id_infer_auto(int privacy, uint8_t *out_addr_type) {
  if (ble_hs_id_copy_addr(BLE_ADDR_RANDOM, nullptr, nullptr) != 0) {
    return BLE_HS_ENOADDR;
  }

  *out_addr_type = BLE_OWN_ADDR_RANDOM;

  return 0;
}

int ble_hs_util_ensure_addr(int prefer_random) {
  uint8_t addr[6];

  if (ble_hs_id_copy_addr(BLE_ADDR_RANDOM, nullptr, nullptr) == 0) {
    return 0;
  }

  if (aci_link_identity(addr) != 0) {
    return BLE_HS_ENOADDR;
  }

  return ble_hs_id_set_rnd(addr);
}

// --- NimBLE's transport, which the port starts -------------------------------------------------

void ble_transport_init(void) {
}

void ble_transport_hs_init(void) {
  ble_npl_event_init(&s_start_ev, prv_start_fn, nullptr);
  ble_npl_event_init(&s_stop_ev, prv_stop_fn, nullptr);
  ble_npl_event_init(&s_reset_ev, prv_reset_fn, nullptr);

  aci_gap_init();
  aci_gatts_init();
  aci_gattc_init();
}

void ble_transport_ll_init(void) {
  aci_link_init();
}
