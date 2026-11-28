/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * GAP and the security manager: advertising, as standard HCI LE commands, the connections the
 * chip reports, their parameters, and the pairing the chip's stack runs, turned into NimBLE's GAP
 * events. Events go to the listeners, newest first, then to the connection's callback, as in
 * NimBLE.
 */

#include "aci_priv.h"

#include <errno.h>

#include <pbl/bluetooth/types.h>
#include <pbl/drivers/rng.h>
#include <pbl/logging/logging.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#include <host/ble_hs_adv.h>
#include <host/ble_sm.h>
#include <host/ble_store.h>
#include <nimble/nimble_port.h>
#include <os/queue.h>

PBL_LOG_MODULE_DECLARE(nimble_aci, CONFIG_NIMBLE_ACI_BLUENRG2_LOG_LEVEL);

#define MAX_CONNS MYNEWT_VAL(BLE_MAX_CONNECTIONS)

#define DEVICE_NAME_MAX 32U
#define ATT_MTU_DEFAULT 23U

#define ADV_IND         0x00U
#define ADV_DIRECT_HIGH 0x01U
#define ADV_SCAN_IND    0x02U
#define ADV_NONCONN_IND 0x03U
#define ADV_DIRECT_LOW  0x04U

// The time a pairing's encryption waits for the pairing to end, should its event not come
#define ENC_WAIT_MS 5000U

struct conn {
  bool used;
  struct ble_gap_conn_desc desc;
  ble_gap_event_fn *cb;
  void *cb_arg;
  uint16_t mtu;
  // A pairing is being run: the encryption is reported once it is over
  bool pairing;
  bool enc_pending;
  bool update_pending;
};

static SLIST_HEAD(, ble_gap_event_listener) s_listeners;
static struct conn s_conns[MAX_CONNS];

static bool s_adv_active;
static bool s_adv_has_rsp;
static ble_gap_event_fn *s_adv_cb;
static void *s_adv_arg;
static struct ble_npl_callout s_adv_timer;
static struct ble_npl_callout s_enc_timer;

static uint16_t s_gap_svc;
static uint16_t s_name_chr;

static int prv_cmd(uint16_t op, const void *par, uint8_t len) {
  return aci_hs_err(aci_cmd(op, par, len, nullptr, nullptr));
}

static struct conn *prv_find(uint16_t handle) {
  for (size_t i = 0U; i < MAX_CONNS; i++) {
    if (s_conns[i].used && s_conns[i].desc.conn_handle == handle) {
      return &s_conns[i];
    }
  }

  return nullptr;
}

bool aci_gap_conn_exists(uint16_t conn_handle) {
  aci_lock();
  bool ok = prv_find(conn_handle) != nullptr;
  aci_unlock();

  return ok;
}

void aci_gap_name_handles(uint16_t *svc, uint16_t *name_chr) {
  *svc = s_gap_svc;
  *name_chr = s_name_chr;
}

// --- Events ------------------------------------------------------------------------------------

static void prv_call_listeners(struct ble_gap_event *event) {
  struct ble_gap_event_listener *evl;

  SLIST_FOREACH(evl, &s_listeners, link) {
    evl->fn(event, evl->arg);
  }
}

static int prv_call(struct ble_gap_event *event, ble_gap_event_fn *cb, void *arg) {
  prv_call_listeners(event);

  return cb != nullptr ? cb(event, arg) : 0;
}

int aci_gap_event(struct ble_gap_event *event, uint16_t conn_handle) {
  ble_gap_event_fn *cb = nullptr;
  void *arg = nullptr;

  aci_lock();
  struct conn *c = prv_find(conn_handle);
  if (c != nullptr) {
    cb = c->cb;
    arg = c->cb_arg;
  }
  aci_unlock();

  return prv_call(event, cb, arg);
}

int ble_gap_event_listener_register(struct ble_gap_event_listener *listener, ble_gap_event_fn *fn,
                                    void *arg) {
  struct ble_gap_event_listener *evl;

  if (fn == nullptr) {
    return BLE_HS_EINVAL;
  }

  SLIST_FOREACH(evl, &s_listeners, link) {
    if (evl == listener) {
      return BLE_HS_EALREADY;
    }
  }

  listener->fn = fn;
  listener->arg = arg;
  SLIST_INSERT_HEAD(&s_listeners, listener, link);

  return 0;
}

int ble_gap_event_listener_unregister(struct ble_gap_event_listener *listener) {
  struct ble_gap_event_listener *evl;

  SLIST_FOREACH(evl, &s_listeners, link) {
    if (evl == listener) {
      SLIST_REMOVE(&s_listeners, listener, ble_gap_event_listener, link);
      return 0;
    }
  }

  return BLE_HS_ENOENT;
}

// --- Addresses and bonds -----------------------------------------------------------------------

static bool prv_is_rpa(const ble_addr_t *addr) {
  return addr->type == BLE_ADDR_RANDOM && (addr->val[5] & 0xc0U) == 0x40U;
}

// The identity address of a peer: its own, or the one its resolvable address resolves to. Whether
// the chip has a bond with it.
static bool prv_identity(const ble_addr_t *ota, ble_addr_t *id) {
  ble_addr_t bonds[12];
  uint8_t rsp[6];
  size_t len = sizeof(rsp);
  bool resolved = false;
  int num;

  *id = *ota;
  if (prv_is_rpa(ota) && aci_cmd(ACI_OP_GAP_RESOLVE_PRIV_ADDR, ota->val, 6U, rsp, &len) == 0 &&
      len >= 6U) {
    memcpy(id->val, rsp, 6U);
    resolved = true;
  }

  num = aci_chip_bonds(bonds, PBL_ARRAY_SIZE(bonds));
  for (int i = 0; i < num; i++) {
    // Its type is the bond's
    if (resolved) {
      id->type = bonds[i].type;
    }
    if (aci_chip_bond_match(&bonds[i], id)) {
      return true;
    }
  }
  if (resolved) {
    id->type = BLE_ADDR_PUBLIC;
  }

  return false;
}

// --- Set-up ------------------------------------------------------------------------------------

static uint8_t prv_io_cap(void) {
  switch (ble_hs_cfg.sm_io_cap) {
    case BLE_HS_IO_DISPLAY_ONLY:
      return 0x00U;
    case BLE_HS_IO_DISPLAY_YESNO:
      return 0x01U;
    case BLE_HS_IO_KEYBOARD_ONLY:
      return 0x02U;
    case BLE_HS_IO_KEYBOARD_DISPLAY:
      return 0x04U;
    default:
      return 0x03U;
  }
}

int aci_gap_setup(void) {
  // Peripheral, no privacy, room for the device name
  static const uint8_t s_init[] = {0x01U, 0x00U, DEVICE_NAME_MAX};
  uint8_t rsp[6];
  size_t len = sizeof(rsp);
  uint8_t auth[12];
  uint8_t io;
  int rc;

  rc = aci_cmd(ACI_OP_GAP_INIT, s_init, sizeof(s_init), rsp, &len);
  if (rc != 0 || len < 6U) {
    PBL_LOG_ERR("GAP init failed (%d)", rc);
    return rc != 0 ? rc : -EIO;
  }
  s_gap_svc = aci_get_le16(rsp);
  s_name_chr = aci_get_le16(&rsp[2]);

  // The chip's Device Name and Appearance are writable by peers; NimBLE's are read only
  for (size_t i = 1U; i <= 2U; i++) {
    uint8_t par[5];
    uint8_t *p = aci_put_le16(par, s_gap_svc);

    p = aci_put_le16(p, aci_get_le16(&rsp[i * 2U]) + 1U);
    *p = 0x01U;
    rc = aci_cmd(ACI_OP_GATT_SET_ACCESS_PERM, par, sizeof(par), nullptr, nullptr);
    if (rc != 0) {
      PBL_LOG_WRN("GAP characteristic %04x left writable (%d)", aci_get_le16(&rsp[i * 2U]), rc);
    }
  }

  io = prv_io_cap();
  rc = aci_cmd(ACI_OP_GAP_SET_IO_CAP, &io, 1U, nullptr, nullptr);
  if (rc != 0) {
    PBL_LOG_ERR("IO capability refused (%d)", rc);
    return rc;
  }

  auth[0] = ble_hs_cfg.sm_bonding ? 0x01U : 0x00U;
  auth[1] = ble_hs_cfg.sm_mitm ? 0x01U : 0x00U;
  auth[2] = !ble_hs_cfg.sm_sc ? 0x00U : (MYNEWT_VAL(BLE_SM_SC_ONLY) ? 0x02U : 0x01U);
  auth[3] = ble_hs_cfg.sm_keypress ? 0x01U : 0x00U;
  // Secure connections take 16-byte keys
  auth[4] = MYNEWT_VAL(BLE_SM_SC_ONLY) ? 16U : 7U;
  auth[5] = 16U;
  // No fixed passkey: the host is asked for one
  auth[6] = 0x01U;
  (void)aci_put_le32(&auth[7], 0U);
  auth[11] = 0x01U;
  rc = aci_cmd(ACI_OP_GAP_SET_AUTH_REQ, auth, sizeof(auth), nullptr, nullptr);
  if (rc != 0) {
    PBL_LOG_ERR("Authentication requirements refused (%d)", rc);
    return rc;
  }

  aci_lock();
  memset(s_conns, 0, sizeof(s_conns));
  s_adv_active = false;
  aci_unlock();

  return 0;
}

static void prv_adv_timer_fn(struct ble_npl_event *ev);
static void prv_enc_timer_fn(struct ble_npl_event *ev);

void aci_gap_init(void) {
  SLIST_INIT(&s_listeners);
  ble_npl_callout_init(&s_adv_timer, nimble_port_get_dflt_eventq(), prv_adv_timer_fn, nullptr);
  ble_npl_callout_init(&s_enc_timer, nimble_port_get_dflt_eventq(), prv_enc_timer_fn, nullptr);
}

// --- Connections -------------------------------------------------------------------------------

static void prv_disconnected(struct conn *c, int reason) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_DISCONNECT};
  ble_gap_event_fn *cb = c->cb;
  void *arg = c->cb_arg;

  aci_gatts_conn_closed(c->desc.conn_handle);
  aci_gattc_conn_closed(c->desc.conn_handle);

  aci_lock();
  event.disconnect.conn = c->desc;
  c->used = false;
  aci_unlock();

  event.disconnect.reason = reason;
  (void)prv_call(&event, cb, arg);
}

void aci_gap_reset(int reason) {
  ble_npl_callout_stop(&s_adv_timer);
  ble_npl_callout_stop(&s_enc_timer);
  s_adv_active = false;

  for (size_t i = 0U; i < MAX_CONNS; i++) {
    if (s_conns[i].used) {
      prv_disconnected(&s_conns[i], reason);
    }
  }
}

static void prv_rx_conn_complete(const uint8_t *p, size_t n, bool enhanced) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_CONNECT};
  uint8_t status = p[0];
  uint16_t handle = aci_get_le16(&p[1]);
  const uint8_t *tail = enhanced ? &p[23] : &p[11];
  struct conn *c = nullptr;
  ble_gap_event_fn *cb;
  void *arg;

  if ((size_t)(tail - p) + 7U > n) {
    return;
  }

  cb = s_adv_cb;
  arg = s_adv_arg;
  // Legacy advertising ends with a connection, or with an error to the advertiser
  s_adv_active = false;
  ble_npl_callout_stop(&s_adv_timer);

  if (status != 0U) {
    event.connect.status = BLE_HS_HCI_ERR(status);
    event.connect.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    (void)prv_call(&event, cb, arg);
    return;
  }

  aci_lock();
  for (size_t i = 0U; i < MAX_CONNS && c == nullptr; i++) {
    if (!s_conns[i].used) {
      c = &s_conns[i];
    }
  }
  if (c != nullptr) {
    *c = (struct conn){
      .used = true,
      .cb = cb,
      .cb_arg = arg,
      .mtu = ATT_MTU_DEFAULT,
    };
    c->desc.conn_handle = handle;
    c->desc.role = p[3] == 0x01U ? BLE_GAP_ROLE_SLAVE : BLE_GAP_ROLE_MASTER;
    c->desc.peer_ota_addr.type = p[4] & 0x01U;
    memcpy(c->desc.peer_ota_addr.val, &p[5], 6U);
    c->desc.conn_itvl = aci_get_le16(tail);
    c->desc.conn_latency = aci_get_le16(&tail[2]);
    c->desc.supervision_timeout = aci_get_le16(&tail[4]);
    c->desc.master_clock_accuracy = tail[6];
    c->desc.our_id_addr.type = BLE_ADDR_RANDOM;
    (void)ble_hs_id_copy_addr(BLE_ADDR_RANDOM, c->desc.our_id_addr.val, nullptr);
    c->desc.our_ota_addr = c->desc.our_id_addr;
  }
  aci_unlock();

  if (c == nullptr) {
    PBL_LOG_ERR("No room for connection %04x", handle);
    (void)ble_gap_terminate(handle, BLE_ERR_CONN_LIMIT);
    return;
  }

  // Out of the lock: these are commands
  ble_addr_t id;
  bool bonded;

  bonded = prv_identity(&c->desc.peer_ota_addr, &id);
  aci_lock();
  c->desc.peer_id_addr = id;
  c->desc.sec_state.bonded = bonded;
  aci_unlock();

  PBL_LOG_DBG("Connected %04x, interval %u, %s", handle, c->desc.conn_itvl,
              bonded ? "bonded" : "not bonded");

  event.connect.status = 0;
  event.connect.conn_handle = handle;
  (void)prv_call(&event, cb, arg);
}

static void prv_rx_conn_update(const uint8_t *p, size_t n) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_CONN_UPDATE};
  uint16_t handle;
  struct conn *c;

  if (n < 9U) {
    return;
  }
  handle = aci_get_le16(&p[1]);

  aci_lock();
  c = prv_find(handle);
  if (c != nullptr) {
    c->update_pending = false;
    if (p[0] == 0U) {
      c->desc.conn_itvl = aci_get_le16(&p[3]);
      c->desc.conn_latency = aci_get_le16(&p[5]);
      c->desc.supervision_timeout = aci_get_le16(&p[7]);
    }
  }
  aci_unlock();

  if (c == nullptr) {
    return;
  }

  PBL_LOG_DBG("Connection %04x updated: status %u, interval %u, latency %u", handle, p[0],
              aci_get_le16(&p[3]), aci_get_le16(&p[5]));
  event.conn_update.status = BLE_HS_HCI_ERR(p[0]);
  event.conn_update.conn_handle = handle;
  (void)aci_gap_event(&event, handle);
}

static void prv_enc_event(struct conn *c, int status) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_ENC_CHANGE};

  c->enc_pending = false;
  event.enc_change.conn_handle = c->desc.conn_handle;
  event.enc_change.status = status;
  (void)aci_gap_event(&event, c->desc.conn_handle);
}

static void prv_rx_enc_change(uint8_t status, uint16_t handle, bool enabled) {
  uint8_t par[2];
  uint8_t rsp[2] = {};
  size_t len = sizeof(rsp);
  struct conn *c;
  bool bonded;

  aci_lock();
  c = prv_find(handle);
  aci_unlock();
  if (c == nullptr) {
    return;
  }

  if (status != 0U || !enabled) {
    aci_lock();
    c->desc.sec_state.encrypted = 0;
    aci_unlock();
    prv_enc_event(c, status != 0U ? BLE_HS_HCI_ERR(status) : BLE_HS_EENCRYPT);
    return;
  }

  (void)aci_put_le16(par, handle);
  ble_addr_t id;
  bonded = prv_identity(&c->desc.peer_ota_addr, &id);
  aci_lock();
  c->desc.peer_id_addr = id;
  c->desc.sec_state.encrypted = 1;
  c->desc.sec_state.key_size = 16;
  c->desc.sec_state.bonded = bonded;
  aci_unlock();
  if (aci_cmd(ACI_OP_GAP_GET_SECURITY_LEVEL, par, sizeof(par), rsp, &len) == 0 && len >= 2U) {
    aci_lock();
    c->desc.sec_state.authenticated = rsp[1] >= 3U;
    aci_unlock();
  }
  PBL_LOG_DBG("Encrypted %04x: mode %u level %u, %s", handle, rsp[0], rsp[1],
              bonded ? "bonded" : "not bonded");

  // A link encrypted without a bond is being paired: the bond comes after the encryption, and
  // NimBLE reports the encryption once the keys are stored
  if (!bonded || c->pairing) {
    c->pairing = true;
    c->enc_pending = true;
    ble_npl_callout_reset(&s_enc_timer, ble_npl_time_ms_to_ticks32(ENC_WAIT_MS));
    return;
  }

  prv_enc_event(c, 0);
}

static void prv_enc_timer_fn(struct ble_npl_event *ev) {
  for (size_t i = 0U; i < MAX_CONNS; i++) {
    if (s_conns[i].used && s_conns[i].enc_pending) {
      PBL_LOG_WRN("Encryption of %04x reported without the pairing's end",
                  s_conns[i].desc.conn_handle);
      prv_enc_event(&s_conns[i], 0);
    }
  }
}

void aci_gap_rx_hci(uint8_t code, const uint8_t *p, size_t n) {
  struct conn *c;

  switch (code) {
    case ACI_EVT_DISCONN_COMPLETE:
      if (n < 4U) {
        return;
      }
      aci_lock();
      c = prv_find(aci_get_le16(&p[1]));
      aci_unlock();
      if (c != nullptr && p[0] == 0U) {
        PBL_LOG_DBG("Disconnected %04x, reason %02x", c->desc.conn_handle, p[3]);
        prv_disconnected(c, BLE_HS_HCI_ERR(p[3]));
      }
      break;
    case ACI_EVT_ENC_CHANGE:
      if (n >= 4U) {
        prv_rx_enc_change(p[0], aci_get_le16(&p[1]), p[3] != 0U);
      }
      break;
    case ACI_EVT_ENC_KEY_REFRESH:
      if (n >= 3U) {
        prv_rx_enc_change(p[0], aci_get_le16(&p[1]), true);
      }
      break;
    default:
      break;
  }
}

void aci_gap_rx_le(uint8_t sub, const uint8_t *p, size_t n) {
  switch (sub) {
    case ACI_LE_CONN_COMPLETE:
      prv_rx_conn_complete(p, n, false);
      break;
    case ACI_LE_ENH_CONN_COMPLETE:
      prv_rx_conn_complete(p, n, true);
      break;
    case ACI_LE_CONN_UPDATE:
      prv_rx_conn_update(p, n);
      break;
    case ACI_LE_DATA_LEN_CHANGE:
      if (n >= 10U) {
        PBL_LOG_DBG("Data length of %04x: %u bytes out, %u in", aci_get_le16(p),
                    aci_get_le16(&p[2]), aci_get_le16(&p[6]));
      }
      break;
    default:
      break;
  }
}

// --- Pairing -----------------------------------------------------------------------------------

static struct conn *prv_find_pairing(void) {
  for (size_t i = 0U; i < MAX_CONNS; i++) {
    if (s_conns[i].used && s_conns[i].pairing) {
      return &s_conns[i];
    }
  }
  for (size_t i = 0U; i < MAX_CONNS; i++) {
    if (s_conns[i].used) {
      return &s_conns[i];
    }
  }

  return nullptr;
}

// What the host would have asked before the pairing went on: whether one is welcome. The chip
// asks nothing before its first confirmation, which is where the answer goes.
static bool prv_pairing_allowed(struct conn *c) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_PAIRING_REQUEST};

  event.pairing_request.conn_handle = c->desc.conn_handle;

  return aci_gap_event(&event, c->desc.conn_handle) == 0;
}

static void prv_passkey_event(struct conn *c, uint8_t action, uint32_t numcmp) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_PASSKEY_ACTION};

  event.passkey.conn_handle = c->desc.conn_handle;
  event.passkey.params.action = action;
  event.passkey.params.numcmp = numcmp;
  (void)aci_gap_event(&event, c->desc.conn_handle);
}

static void prv_rx_numcmp(const uint8_t *p, size_t n) {
  uint16_t handle = aci_get_le16(p);
  struct conn *c;

  aci_lock();
  c = prv_find(handle);
  if (c != nullptr) {
    c->pairing = true;
  }
  aci_unlock();
  if (c == nullptr) {
    return;
  }

  if (!prv_pairing_allowed(c)) {
    uint8_t par[3];

    (void)aci_put_le16(par, handle);
    par[2] = 0U;
    (void)aci_cmd(ACI_OP_GAP_NUMCMP_CONFIRM, par, sizeof(par), nullptr, nullptr);
    return;
  }

  prv_passkey_event(c, BLE_SM_IOACT_NUMCMP, aci_get_le32(&p[2]));
}

static void prv_rx_passkey_req(const uint8_t *p, size_t n) {
  uint16_t handle = aci_get_le16(p);
  uint32_t passkey = 0U;
  uint8_t par[6];
  struct conn *c;

  aci_lock();
  c = prv_find(handle);
  if (c != nullptr) {
    c->pairing = true;
  }
  aci_unlock();
  if (c == nullptr) {
    return;
  }

  // The peer types the passkey shown here; refused with one the peer cannot know
  if (prv_pairing_allowed(c)) {
    (void)rng_rand(&passkey);
  }
  passkey %= 1000000U;

  (void)aci_put_le16(par, handle);
  (void)aci_put_le32(&par[2], passkey);
  (void)aci_cmd(ACI_OP_GAP_PASS_KEY_RESP, par, sizeof(par), nullptr, nullptr);

  prv_passkey_event(c, BLE_SM_IOACT_DISP, passkey);
}

static void prv_rx_pairing_complete(const uint8_t *p, size_t n) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_PAIRING_COMPLETE};
  uint16_t handle = aci_get_le16(p);
  uint8_t status = p[2];
  uint8_t reason = n >= 4U ? p[3] : 0U;
  struct conn *c;
  int err;

  aci_lock();
  c = prv_find(handle);
  aci_unlock();
  if (c == nullptr) {
    return;
  }

  ble_npl_callout_stop(&s_enc_timer);

  switch (status) {
    case 0x00U:
      err = 0;
      break;
    case 0x01U:
      err = BLE_HS_ETIMEOUT;
      break;
    case 0x02U:
      err = BLE_HS_ERR_SM_US_BASE + (reason != 0U ? reason : BLE_SM_ERR_UNSPECIFIED);
      break;
    default:
      err = BLE_HS_EENCRYPT;
      break;
  }

  PBL_LOG_DBG("Pairing of %04x complete: status %u, reason %u", handle, status, reason);

  // The chip reports a link encrypted again with a bond's keys as a pairing too; NimBLE does not.
  // Failed, the bond is no use: the encryption failed.
  if (!c->pairing) {
    if (err != 0) {
      prv_enc_event(c, err);
    }
    return;
  }

  if (err == 0) {
    ble_addr_t id;
    // The peer's identity is known once its keys are
    bool bonded = prv_identity(&c->desc.peer_ota_addr, &id);

    aci_lock();
    c->desc.peer_id_addr = id;
    c->desc.sec_state.bonded = bonded;
    aci_unlock();
    PBL_LOG_DBG("Paired with " PBL_BT_ADDR_FMT " (type %u), %s", PBL_BT_ADDR_XPLODE_VAL(id.val),
                id.type, bonded ? "bonded" : "no bond on the chip");
    if (bonded) {
      aci_store_bonded(&id, c->desc.sec_state.authenticated);
    }
  }

  c->pairing = false;
  if (c->enc_pending) {
    prv_enc_event(c, err);
  }

  event.pairing_complete.conn_handle = handle;
  event.pairing_complete.status = err;
  (void)aci_gap_event(&event, handle);
}

static void prv_rx_bond_lost(void) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_REPEAT_PAIRING};
  struct conn *c;
  int rc;

  aci_lock();
  c = prv_find_pairing();
  aci_unlock();
  if (c == nullptr) {
    return;
  }

  event.repeat_pairing.conn_handle = c->desc.conn_handle;
  event.repeat_pairing.cur_key_size = c->desc.sec_state.key_size;
  event.repeat_pairing.cur_authenticated = c->desc.sec_state.authenticated;
  event.repeat_pairing.cur_sc = 1;
  event.repeat_pairing.new_key_size = 16;
  event.repeat_pairing.new_authenticated = ble_hs_cfg.sm_mitm;
  event.repeat_pairing.new_sc = ble_hs_cfg.sm_sc;
  event.repeat_pairing.new_bonding = ble_hs_cfg.sm_bonding;
  rc = aci_gap_event(&event, c->desc.conn_handle);

  if (rc == BLE_GAP_REPEAT_PAIRING_RETRY) {
    c->pairing = true;
    (void)aci_cmd(ACI_OP_GAP_ALLOW_REBOND, nullptr, 0U, nullptr, nullptr);
  }
}

int ble_sm_inject_io(uint16_t conn_handle, struct ble_sm_io *pkey) {
  uint8_t par[3];

  if (!aci_gap_conn_exists(conn_handle)) {
    return BLE_HS_ENOTCONN;
  }

  switch (pkey->action) {
    case BLE_SM_IOACT_NUMCMP:
      (void)aci_put_le16(par, conn_handle);
      par[2] = pkey->numcmp_accept ? 1U : 0U;
      return prv_cmd(ACI_OP_GAP_NUMCMP_CONFIRM, par, sizeof(par));
    case BLE_SM_IOACT_DISP:
      // Given to the chip when it asked
      return 0;
    default:
      return BLE_HS_EINVAL;
  }
}

int ble_gap_security_initiate(uint16_t conn_handle) {
  uint8_t par[2];
  bool pairing;

  aci_lock();
  struct conn *c = prv_find(conn_handle);
  pairing = c != nullptr && c->pairing;
  aci_unlock();

  if (c == nullptr) {
    return BLE_HS_ENOTCONN;
  }
  if (pairing) {
    return BLE_HS_EALREADY;
  }

  (void)aci_put_le16(par, conn_handle);

  return prv_cmd(ACI_OP_GAP_SLAVE_SECURITY_REQ, par, sizeof(par));
}

int ble_gap_unpair(const ble_addr_t *peer_addr) {
  for (size_t i = 0U; i < MAX_CONNS; i++) {
    aci_lock();
    bool match = s_conns[i].used && ble_addr_cmp(&s_conns[i].desc.peer_id_addr, peer_addr) == 0;
    uint16_t handle = s_conns[i].desc.conn_handle;
    aci_unlock();

    if (match) {
      (void)ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    }
  }

  return ble_store_util_delete_peer(peer_addr);
}

// --- Vendor events -----------------------------------------------------------------------------

void aci_gap_set_mtu(uint16_t conn_handle, uint16_t mtu) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_MTU};

  aci_lock();
  struct conn *c = prv_find(conn_handle);
  if (c != nullptr) {
    c->mtu = mtu;
  }
  aci_unlock();
  if (c == nullptr) {
    return;
  }

  PBL_LOG_DBG("MTU of %04x: %u", conn_handle, mtu);
  event.mtu.conn_handle = conn_handle;
  event.mtu.channel_id = BLE_L2CAP_CID_ATT;
  event.mtu.value = mtu;
  (void)aci_gap_event(&event, conn_handle);
}

static void prv_rx_l2cap_update(uint16_t handle, int status) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_CONN_UPDATE};

  aci_lock();
  struct conn *c = prv_find(handle);
  if (c != nullptr) {
    c->update_pending = false;
  }
  aci_unlock();
  if (c == nullptr) {
    return;
  }

  event.conn_update.status = status;
  event.conn_update.conn_handle = handle;
  (void)aci_gap_event(&event, handle);
}

bool aci_gap_rx_vs(uint16_t code, const uint8_t *p, size_t n) {
  switch (code) {
    case ACI_VS_GAP_PAIRING_CMPLT:
      if (n >= 3U) {
        prv_rx_pairing_complete(p, n);
      }
      return true;
    case ACI_VS_GAP_NUMCMP_VALUE:
      if (n >= 6U) {
        prv_rx_numcmp(p, n);
      }
      return true;
    case ACI_VS_GAP_PASS_KEY_REQ:
      if (n >= 2U) {
        prv_rx_passkey_req(p, n);
      }
      return true;
    case ACI_VS_GAP_BOND_LOST:
      prv_rx_bond_lost();
      return true;
    case ACI_VS_GAP_SLAVE_SEC_INIT:
    case ACI_VS_GAP_PROC_COMPLETE:
      return true;
    case ACI_VS_L2CAP_UPDATE_RESP:
      // Accepted: the update itself follows. Refused: over.
      if (n >= 4U && aci_get_le16(&p[2]) != 0U) {
        prv_rx_l2cap_update(aci_get_le16(p), BLE_HS_EREJECT);
      }
      return true;
    case ACI_VS_L2CAP_PROC_TIMEOUT:
      if (n >= 2U) {
        prv_rx_l2cap_update(aci_get_le16(p), BLE_HS_ETIMEOUT);
      }
      return true;
    case ACI_VS_ATT_MTU_RESP:
      if (n >= 4U) {
        aci_gap_set_mtu(aci_get_le16(p), aci_get_le16(&p[2]));
      }
      return true;
    default:
      return false;
  }
}

// --- Connection API ----------------------------------------------------------------------------

int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out_desc) {
  int rc = BLE_HS_ENOTCONN;

  aci_lock();
  struct conn *c = prv_find(handle);
  if (c != nullptr) {
    if (out_desc != nullptr) {
      *out_desc = c->desc;
    }
    rc = 0;
  }
  aci_unlock();

  return rc;
}

int ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason) {
  uint8_t par[3];

  if (!aci_gap_conn_exists(conn_handle)) {
    return BLE_HS_ENOTCONN;
  }

  (void)aci_put_le16(par, conn_handle);
  par[2] = hci_reason;

  return prv_cmd(ACI_OP_GAP_TERMINATE, par, sizeof(par));
}

int ble_gap_update_params(uint16_t conn_handle, const struct ble_gap_upd_params *params) {
  uint8_t par[10];
  uint8_t *p;
  int rc;

  aci_lock();
  struct conn *c = prv_find(conn_handle);
  bool pending = c != nullptr && c->update_pending;
  aci_unlock();

  if (c == nullptr) {
    return BLE_HS_ENOTCONN;
  }
  if (pending) {
    return BLE_HS_EALREADY;
  }

  // A peripheral asks over L2CAP: the controller has no parameters request procedure
  p = aci_put_le16(par, conn_handle);
  p = aci_put_le16(p, params->itvl_min);
  p = aci_put_le16(p, params->itvl_max);
  p = aci_put_le16(p, params->latency);
  (void)aci_put_le16(p, params->supervision_timeout);

  rc = prv_cmd(ACI_OP_L2CAP_CONN_PARAM_UPDATE, par, sizeof(par));
  if (rc == 0) {
    aci_lock();
    c->update_pending = true;
    aci_unlock();
  }

  return rc;
}

int ble_gap_write_sugg_def_data_len(uint16_t sugg_max_tx_octets, uint16_t sugg_max_tx_time) {
  uint8_t par[4];

  (void)aci_put_le16(par, sugg_max_tx_octets);
  (void)aci_put_le16(&par[2], sugg_max_tx_time);

  return prv_cmd(ACI_OP_HCI_LE_WRITE_SUGG_DLEN, par, sizeof(par));
}

uint16_t ble_att_mtu(uint16_t conn_handle) {
  uint16_t mtu = 0U;

  aci_lock();
  struct conn *c = prv_find(conn_handle);
  if (c != nullptr) {
    mtu = c->mtu;
  }
  aci_unlock();

  return mtu;
}

// --- Advertising -------------------------------------------------------------------------------

static int prv_set_data(uint16_t op, const uint8_t *data, uint8_t len) {
  uint8_t par[1U + BLE_HS_ADV_MAX_SZ] = {};

  if (len > BLE_HS_ADV_MAX_SZ) {
    return BLE_HS_EINVAL;
  }

  par[0] = len;
  if (len > 0U) {
    memcpy(&par[1], data, len);
  }

  return prv_cmd(op, par, sizeof(par));
}

int ble_gap_adv_set_data(const uint8_t *data, int data_len) {
  return prv_set_data(ACI_OP_HCI_LE_SET_ADV_DATA, data, (uint8_t)data_len);
}

int ble_gap_adv_rsp_set_data(const uint8_t *data, int data_len) {
  int rc = prv_set_data(ACI_OP_HCI_LE_SET_SCAN_RSP, data, (uint8_t)data_len);

  if (rc == 0) {
    s_adv_has_rsp = data_len > 0;
  }

  return rc;
}

int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *adv_fields) {
  uint8_t buf[BLE_HS_ADV_MAX_SZ];
  uint8_t len = 0U;
  int rc;

  rc = ble_hs_adv_set_fields(adv_fields, buf, &len, sizeof(buf));
  if (rc != 0) {
    return rc;
  }

  return ble_gap_adv_set_data(buf, len);
}

int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *rsp_fields) {
  uint8_t buf[BLE_HS_ADV_MAX_SZ];
  uint8_t len = 0U;
  int rc;

  rc = ble_hs_adv_set_fields(rsp_fields, buf, &len, sizeof(buf));
  if (rc != 0) {
    return rc;
  }

  return ble_gap_adv_rsp_set_data(buf, len);
}

int ble_hs_hci_util_read_adv_tx_pwr(int8_t *out_pwr) {
  uint8_t rsp[1];
  size_t len = sizeof(rsp);
  int rc;

  rc = aci_cmd(ACI_OP_HCI_LE_READ_ADV_TX_PWR, nullptr, 0U, rsp, &len);
  if (rc != 0 || len < 1U) {
    return aci_hs_err(rc != 0 ? rc : -EIO);
  }
  *out_pwr = (int8_t)rsp[0];

  return 0;
}

static uint8_t prv_adv_type(const struct ble_gap_adv_params *params) {
  switch (params->conn_mode) {
    case BLE_GAP_CONN_MODE_DIR:
      return params->high_duty_cycle ? ADV_DIRECT_HIGH : ADV_DIRECT_LOW;
    case BLE_GAP_CONN_MODE_UND:
      return ADV_IND;
    default:
      return (params->disc_mode != BLE_GAP_DISC_MODE_NON || s_adv_has_rsp) ? ADV_SCAN_IND
                                                                           : ADV_NONCONN_IND;
  }
}

static void prv_adv_timer_fn(struct ble_npl_event *ev) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_ADV_COMPLETE};
  const uint8_t off = 0U;

  if (!s_adv_active) {
    return;
  }

  (void)prv_cmd(ACI_OP_HCI_LE_SET_ADV_ENABLE, &off, 1U);
  s_adv_active = false;
  event.adv_complete.reason = BLE_HS_ETIMEOUT;
  (void)prv_call(&event, s_adv_cb, s_adv_arg);
}

int ble_gap_adv_start(uint8_t own_addr_type, const ble_addr_t *direct_addr, int32_t duration_ms,
                      const struct ble_gap_adv_params *adv_params, ble_gap_event_fn *cb,
                      void *cb_arg) {
  uint8_t par[15] = {};
  uint8_t *p;
  const uint8_t on = 1U;
  int rc;

  if (!ble_hs_synced()) {
    return BLE_HS_ENOTSYNCED;
  }
  if (s_adv_active) {
    return BLE_HS_EALREADY;
  }

  p = aci_put_le16(par, adv_params->itvl_min != 0U ? adv_params->itvl_min
                                                   : BLE_GAP_ADV_FAST_INTERVAL1_MIN);
  p = aci_put_le16(p, adv_params->itvl_max != 0U ? adv_params->itvl_max
                                                 : BLE_GAP_ADV_FAST_INTERVAL1_MAX);
  *p++ = prv_adv_type(adv_params);
  *p++ = own_addr_type;
  if (direct_addr != nullptr) {
    *p++ = direct_addr->type;
    memcpy(p, direct_addr->val, 6U);
  } else {
    *p++ = 0U;
  }
  p = &par[13];
  *p++ = adv_params->channel_map != 0U ? adv_params->channel_map : BLE_GAP_ADV_DFLT_CHANNEL_MAP;
  *p = adv_params->filter_policy;

  rc = prv_cmd(ACI_OP_HCI_LE_SET_ADV_PARAMS, par, sizeof(par));
  if (rc != 0) {
    return rc;
  }

  s_adv_cb = cb;
  s_adv_arg = cb_arg;
  rc = prv_cmd(ACI_OP_HCI_LE_SET_ADV_ENABLE, &on, 1U);
  if (rc != 0) {
    return rc;
  }
  s_adv_active = true;

  if (duration_ms != BLE_HS_FOREVER) {
    ble_npl_callout_reset(&s_adv_timer, ble_npl_time_ms_to_ticks32((uint32_t)duration_ms));
  }

  return 0;
}

int ble_gap_adv_stop(void) {
  const uint8_t off = 0U;
  int rc;

  if (!s_adv_active) {
    return BLE_HS_EALREADY;
  }

  ble_npl_callout_stop(&s_adv_timer);
  rc = prv_cmd(ACI_OP_HCI_LE_SET_ADV_ENABLE, &off, 1U);
  if (rc == 0) {
    s_adv_active = false;
  }

  return rc;
}

int ble_gap_adv_active(void) {
  return s_adv_active;
}
