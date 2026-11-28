/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The GATT client: NimBLE's procedures as the chip's, which runs one at a time on a connection;
 * the others wait their turn. Their results come back as ATT response events, then a
 * procedure-complete event, and go to the callbacks the way NimBLE calls them.
 */

#include "aci_priv.h"
#include "gatts_map.h"

#include <pbl/logging/logging.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#include <host/ble_att.h>
#include <host/ble_hs_mbuf.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>

PBL_LOG_MODULE_DECLARE(nimble_aci, CONFIG_NIMBLE_ACI_BLUENRG2_LOG_LEVEL);

#define MAX_PROCS MYNEWT_VAL(BLE_GATT_MAX_PROCS)

enum proc_type {
  PROC_DISC_SVC_UUID,
  PROC_DISC_ALL_CHRS,
  PROC_DISC_CHRS_UUID,
  PROC_DISC_ALL_DSCS,
  PROC_READ,
  PROC_READ_LONG,
  PROC_READ_UUID,
  PROC_WRITE,
};

struct proc {
  bool used;
  bool started;
  uint8_t type;
  uint16_t conn_handle;
  uint16_t start;
  uint16_t end;
  uint16_t handle;
  uint16_t offset;
  ble_uuid_any_t uuid;
  void *cb;
  void *arg;
  // A write's data, a read's value
  struct os_mbuf *om;
  uint8_t att_err;
  // Order of the queue
  uint32_t seq;
};

static struct proc s_procs[MAX_PROCS];
static uint32_t s_seq;

static struct ble_gatt_error *prv_error(int status, uint16_t att_handle) {
  static struct ble_gatt_error s_error;

  s_error.status = status;
  s_error.att_handle = (status == 0 || status == BLE_HS_EDONE) ? 0U : att_handle;

  return &s_error;
}

// The connection's running procedure
static struct proc *prv_active(uint16_t conn_handle) {
  for (size_t i = 0U; i < MAX_PROCS; i++) {
    if (s_procs[i].used && s_procs[i].started && s_procs[i].conn_handle == conn_handle) {
      return &s_procs[i];
    }
  }

  return nullptr;
}

static struct proc *prv_next(uint16_t conn_handle) {
  struct proc *next = nullptr;

  for (size_t i = 0U; i < MAX_PROCS; i++) {
    struct proc *p = &s_procs[i];

    if (p->used && !p->started && p->conn_handle == conn_handle &&
        (next == nullptr || p->seq < next->seq)) {
      next = p;
    }
  }

  return next;
}

// --- Callbacks ---------------------------------------------------------------------------------

static void prv_cb_svc(struct proc *p, int status, const struct ble_gatt_svc *svc) {
  if (p->cb != nullptr) {
    ((ble_gatt_disc_svc_fn *)p->cb)(p->conn_handle, prv_error(status, p->handle), svc, p->arg);
  }
}

static void prv_cb_chr(struct proc *p, int status, const struct ble_gatt_chr *chr) {
  if (p->cb != nullptr) {
    ((ble_gatt_chr_fn *)p->cb)(p->conn_handle, prv_error(status, p->handle), chr, p->arg);
  }
}

static void prv_cb_dsc(struct proc *p, int status, const struct ble_gatt_dsc *dsc) {
  if (p->cb != nullptr) {
    ((ble_gatt_dsc_fn *)p->cb)(p->conn_handle, prv_error(status, p->handle), p->start, dsc, p->arg);
  }
}

// Any value is the callback's, which may keep it by taking attr->om
static void prv_cb_attr(struct proc *p, int status, uint16_t handle, uint16_t offset,
                        struct os_mbuf *om) {
  struct ble_gatt_attr attr = {.handle = handle, .offset = offset, .om = om};

  if (p->cb != nullptr) {
    ((ble_gatt_attr_fn *)p->cb)(
        p->conn_handle, prv_error(status, handle),
        (om != nullptr || status == 0 || p->type == PROC_WRITE) ? &attr : nullptr, p->arg);
  }
  os_mbuf_free_chain(attr.om);
}

// The procedure's end, with its status, and the connection's next one started
static void prv_start_next(uint16_t conn_handle);

static void prv_finish(struct proc *p, int status) {
  struct proc done = *p;

  aci_lock();
  p->used = false;
  p->om = nullptr;
  aci_unlock();

  switch (done.type) {
    case PROC_DISC_SVC_UUID:
      prv_cb_svc(&done, status == 0 ? BLE_HS_EDONE : status, nullptr);
      break;
    case PROC_DISC_ALL_CHRS:
    case PROC_DISC_CHRS_UUID:
      prv_cb_chr(&done, status == 0 ? BLE_HS_EDONE : status, nullptr);
      break;
    case PROC_DISC_ALL_DSCS:
      prv_cb_dsc(&done, status == 0 ? BLE_HS_EDONE : status, nullptr);
      break;
    case PROC_READ:
      prv_cb_attr(&done, status, done.handle, 0U, status == 0 ? done.om : nullptr);
      if (status != 0) {
        os_mbuf_free_chain(done.om);
      }
      done.om = nullptr;
      break;
    case PROC_READ_LONG:
    case PROC_READ_UUID:
      prv_cb_attr(&done, status == 0 ? BLE_HS_EDONE : status, done.handle, 0U, nullptr);
      break;
    case PROC_WRITE:
      prv_cb_attr(&done, status, done.handle, 0U, nullptr);
      break;
    default:
      break;
  }
  os_mbuf_free_chain(done.om);

  prv_start_next(done.conn_handle);
}

// --- Starting ----------------------------------------------------------------------------------

static uint8_t *prv_put_uuid(uint8_t *q, const ble_uuid_t *uuid) {
  uint8_t type = aci_map_uuid(uuid, &q[1]);

  q[0] = type;

  return q + 1U + (type == ACI_UUID_16 ? 2U : 16U);
}

static int prv_send(struct proc *p) {
  uint8_t buf[ACI_MAX_PARAMS];
  uint8_t *q = aci_put_le16(buf, p->conn_handle);
  uint16_t op;

  switch (p->type) {
    case PROC_DISC_SVC_UUID:
      op = ACI_OP_GATT_DISC_SVC_BY_UUID;
      q = prv_put_uuid(q, &p->uuid.u);
      break;
    case PROC_DISC_ALL_CHRS:
      op = ACI_OP_GATT_DISC_ALL_CHARS;
      q = aci_put_le16(q, p->start);
      q = aci_put_le16(q, p->end);
      break;
    case PROC_DISC_CHRS_UUID:
      op = ACI_OP_GATT_DISC_CHARS_BY_UUID;
      q = aci_put_le16(q, p->start);
      q = aci_put_le16(q, p->end);
      q = prv_put_uuid(q, &p->uuid.u);
      break;
    case PROC_DISC_ALL_DSCS:
      op = ACI_OP_GATT_DISC_ALL_DESCS;
      q = aci_put_le16(q, p->start);
      q = aci_put_le16(q, p->end);
      break;
    case PROC_READ:
      op = ACI_OP_GATT_READ_CHAR_VALUE;
      q = aci_put_le16(q, p->handle);
      break;
    case PROC_READ_LONG:
      op = ACI_OP_GATT_READ_LONG;
      q = aci_put_le16(q, p->handle);
      q = aci_put_le16(q, p->offset);
      break;
    case PROC_READ_UUID:
      op = ACI_OP_GATT_READ_BY_UUID;
      q = aci_put_le16(q, p->start);
      q = aci_put_le16(q, p->end);
      q = prv_put_uuid(q, &p->uuid.u);
      break;
    case PROC_WRITE: {
      uint16_t len = OS_MBUF_PKTLEN(p->om);
      bool is_long = len > ble_att_mtu(p->conn_handle) - 3U;

      op = is_long ? ACI_OP_GATT_WRITE_LONG : ACI_OP_GATT_WRITE_CHAR_VALUE;
      q = aci_put_le16(q, p->handle);
      if (is_long) {
        q = aci_put_le16(q, 0U);
      }
      if (len > (size_t)(&buf[sizeof(buf)] - q) - 1U) {
        return BLE_HS_EMSGSIZE;
      }
      *q++ = (uint8_t)len;
      (void)os_mbuf_copydata(p->om, 0, len, q);
      q += len;
      break;
    }
    default:
      return BLE_HS_EINVAL;
  }

  return aci_hs_err(aci_cmd(op, buf, (uint8_t)(q - buf), nullptr, nullptr));
}

static void prv_start_next(uint16_t conn_handle) {
  while (true) {
    struct proc *p;
    int rc;

    aci_lock();
    p = prv_active(conn_handle) == nullptr ? prv_next(conn_handle) : nullptr;
    if (p != nullptr) {
      p->started = true;
      p->att_err = 0U;
    }
    aci_unlock();

    if (p == nullptr) {
      return;
    }

    rc = prv_send(p);
    if (rc == 0) {
      return;
    }

    // Refused at once: reported as NimBLE would have, from the callback
    PBL_LOG_DBG("GATT procedure %u refused: %d", p->type, rc);
    prv_finish(p, rc);
  }
}

static int prv_queue(uint8_t type, uint16_t conn_handle, uint16_t start, uint16_t end,
                     uint16_t handle, uint16_t offset, const ble_uuid_t *uuid, void *cb, void *arg,
                     struct os_mbuf *om) {
  struct proc *p = nullptr;

  if (!aci_gap_conn_exists(conn_handle)) {
    os_mbuf_free_chain(om);
    return BLE_HS_ENOTCONN;
  }

  aci_lock();
  for (size_t i = 0U; i < MAX_PROCS && p == nullptr; i++) {
    if (!s_procs[i].used) {
      p = &s_procs[i];
    }
  }
  if (p != nullptr) {
    *p = (struct proc){
      .used = true,
      .type = type,
      .conn_handle = conn_handle,
      .start = start,
      .end = end,
      .handle = handle,
      .offset = offset,
      .cb = cb,
      .arg = arg,
      .om = om,
      .seq = s_seq++,
    };
    if (uuid != nullptr) {
      ble_uuid_copy(&p->uuid, uuid);
    }
  }
  aci_unlock();

  if (p == nullptr) {
    os_mbuf_free_chain(om);
    return BLE_HS_ENOMEM;
  }

  prv_start_next(conn_handle);

  return 0;
}

// --- API ---------------------------------------------------------------------------------------

int ble_gattc_disc_svc_by_uuid(uint16_t conn_handle, const ble_uuid_t *uuid,
                               ble_gatt_disc_svc_fn *cb, void *cb_arg) {
  return prv_queue(PROC_DISC_SVC_UUID, conn_handle, 0U, 0U, 0U, 0U, uuid, cb, cb_arg, nullptr);
}

int ble_gattc_disc_all_chrs(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle,
                            ble_gatt_chr_fn *cb, void *cb_arg) {
  return prv_queue(PROC_DISC_ALL_CHRS, conn_handle, start_handle, end_handle, 0U, 0U, nullptr, cb,
                   cb_arg, nullptr);
}

int ble_gattc_disc_chrs_by_uuid(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle,
                                const ble_uuid_t *uuid, ble_gatt_chr_fn *cb, void *cb_arg) {
  return prv_queue(PROC_DISC_CHRS_UUID, conn_handle, start_handle, end_handle, 0U, 0U, uuid, cb,
                   cb_arg, nullptr);
}

int ble_gattc_disc_all_dscs(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle,
                            ble_gatt_dsc_fn *cb, void *cb_arg) {
  return prv_queue(PROC_DISC_ALL_DSCS, conn_handle, start_handle, end_handle, 0U, 0U, nullptr, cb,
                   cb_arg, nullptr);
}

int ble_gattc_read(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_attr_fn *cb, void *cb_arg) {
  return prv_queue(PROC_READ, conn_handle, 0U, 0U, attr_handle, 0U, nullptr, cb, cb_arg, nullptr);
}

int ble_gattc_read_long(uint16_t conn_handle, uint16_t handle, uint16_t offset,
                        ble_gatt_attr_fn *cb, void *cb_arg) {
  return prv_queue(PROC_READ_LONG, conn_handle, 0U, 0U, handle, offset, nullptr, cb, cb_arg,
                   nullptr);
}

int ble_gattc_read_by_uuid(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle,
                           const ble_uuid_t *uuid, ble_gatt_attr_fn *cb, void *cb_arg) {
  return prv_queue(PROC_READ_UUID, conn_handle, start_handle, end_handle, 0U, 0U, uuid, cb, cb_arg,
                   nullptr);
}

int ble_gattc_write(uint16_t conn_handle, uint16_t attr_handle, struct os_mbuf *om,
                    ble_gatt_attr_fn *cb, void *cb_arg) {
  return prv_queue(PROC_WRITE, conn_handle, 0U, 0U, attr_handle, 0U, nullptr, cb, cb_arg, om);
}

int ble_gattc_write_flat(uint16_t conn_handle, uint16_t attr_handle, const void *data,
                         uint16_t data_len, ble_gatt_attr_fn *cb, void *cb_arg) {
  struct os_mbuf *om = ble_hs_mbuf_from_flat(data, data_len);

  if (om == nullptr) {
    return BLE_HS_ENOMEM;
  }

  return ble_gattc_write(conn_handle, attr_handle, om, cb, cb_arg);
}

int ble_gattc_write_no_rsp_flat(uint16_t conn_handle, uint16_t attr_handle, const void *data,
                                uint16_t data_len) {
  uint8_t buf[ACI_MAX_PARAMS];
  uint8_t *q;

  if (!aci_gap_conn_exists(conn_handle)) {
    return BLE_HS_ENOTCONN;
  }
  data_len = PBL_MIN(data_len, (uint16_t)(ble_att_mtu(conn_handle) - 3U));
  if (data_len > sizeof(buf) - 5U) {
    return BLE_HS_EMSGSIZE;
  }

  q = aci_put_le16(buf, conn_handle);
  q = aci_put_le16(q, attr_handle);
  *q++ = (uint8_t)data_len;
  memcpy(q, data, data_len);
  q += data_len;

  return aci_hs_err(aci_cmd(ACI_OP_GATT_WRITE_NO_RSP, buf, (uint8_t)(q - buf), nullptr, nullptr));
}

int ble_gattc_write_no_rsp(uint16_t conn_handle, uint16_t attr_handle, struct os_mbuf *om) {
  uint8_t buf[ACI_CHR_VALUE_MAX];
  uint16_t len = (uint16_t)PBL_MIN(OS_MBUF_PKTLEN(om), (uint32_t)sizeof(buf));
  int rc;

  (void)os_mbuf_copydata(om, 0, len, buf);
  os_mbuf_free_chain(om);
  rc = ble_gattc_write_no_rsp_flat(conn_handle, attr_handle, buf, len);

  return rc;
}

int ble_gattc_exchange_mtu(uint16_t conn_handle, ble_gatt_mtu_fn *cb, void *cb_arg) {
  uint8_t par[2];

  (void)aci_put_le16(par, conn_handle);

  return aci_hs_err(aci_cmd(ACI_OP_GATT_EXCHANGE_CONFIG, par, sizeof(par), nullptr, nullptr));
}

// --- Events, on the host's thread --------------------------------------------------------------

static void prv_rx_find_by_type(struct proc *p, const uint8_t *d, size_t n) {
  uint8_t num = d[2];

  for (size_t i = 0U; i < num && 3U + (i + 1U) * 4U <= n; i++) {
    struct ble_gatt_svc svc = {
      .start_handle = aci_get_le16(&d[3U + i * 4U]),
      .end_handle = aci_get_le16(&d[5U + i * 4U]),
    };

    ble_uuid_copy(&svc.uuid, &p->uuid.u);
    prv_cb_svc(p, 0, &svc);
  }
}

static void prv_rx_read_by_type(struct proc *p, const uint8_t *d, size_t n) {
  uint8_t pair_len = d[2];
  size_t len = PBL_MIN((size_t)d[3], n - 4U);
  const uint8_t *pairs = &d[4];

  if (pair_len != 7U && pair_len != 21U) {
    return;
  }

  for (size_t off = 0U; off + pair_len <= len; off += pair_len) {
    const uint8_t *e = &pairs[off];
    struct ble_gatt_chr chr = {
      .def_handle = aci_get_le16(e),
      .properties = e[2],
      .val_handle = aci_get_le16(&e[3]),
    };

    (void)ble_uuid_init_from_buf(&chr.uuid, &e[5], pair_len - 5U);
    prv_cb_chr(p, 0, &chr);
  }
}

static void prv_rx_find_info(struct proc *p, const uint8_t *d, size_t n) {
  uint8_t format = d[2];
  size_t len = PBL_MIN((size_t)d[3], n - 4U);
  size_t pair_len = format == 1U ? 4U : 18U;
  const uint8_t *pairs = &d[4];

  for (size_t off = 0U; off + pair_len <= len; off += pair_len) {
    struct ble_gatt_dsc dsc = {.handle = aci_get_le16(&pairs[off])};

    if (dsc.handle <= p->start) {
      continue;
    }
    (void)ble_uuid_init_from_buf(&dsc.uuid, &pairs[off + 2U], pair_len - 2U);
    prv_cb_dsc(p, 0, &dsc);
  }
}

static void prv_rx_value(struct proc *p, const uint8_t *val, size_t len) {
  switch (p->type) {
    case PROC_READ:
      if (p->om == nullptr) {
        p->om = ble_hs_mbuf_from_flat(val, (uint16_t)len);
      } else {
        (void)os_mbuf_append(p->om, val, (uint16_t)len);
      }
      break;
    case PROC_READ_LONG:
      prv_cb_attr(p, 0, p->handle, p->offset, ble_hs_mbuf_from_flat(val, (uint16_t)len));
      p->offset += (uint16_t)len;
      break;
    default:
      break;
  }
}

static void prv_rx_notify(const uint8_t *d, size_t n, bool indication) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_NOTIFY_RX};
  uint16_t conn_handle = aci_get_le16(d);
  size_t len = PBL_MIN((size_t)d[4], n - 5U);

  event.notify_rx.conn_handle = conn_handle;
  event.notify_rx.attr_handle = aci_get_le16(&d[2]);
  event.notify_rx.om = ble_hs_mbuf_from_flat(&d[5], (uint16_t)len);
  event.notify_rx.indication = indication;
  if (event.notify_rx.om != nullptr) {
    (void)aci_gap_event(&event, conn_handle);
    os_mbuf_free_chain(event.notify_rx.om);
  }

  if (indication) {
    uint8_t par[2];

    (void)aci_put_le16(par, conn_handle);
    (void)aci_cmd(ACI_OP_GATT_CONFIRM_INDICATION, par, sizeof(par), nullptr, nullptr);
  }
}

// An ATT error that ends a discovery is how it ends
static bool prv_is_discovery(const struct proc *p) {
  return p->type <= PROC_DISC_ALL_DSCS || p->type == PROC_READ_UUID;
}

static void prv_rx_complete(struct proc *p, uint8_t error_code) {
  int status = 0;

  if (p->att_err != 0U && !(prv_is_discovery(p) && p->att_err == BLE_ATT_ERR_ATTR_NOT_FOUND)) {
    status = BLE_HS_ATT_ERR(p->att_err);
  } else if (error_code != 0U) {
    status = BLE_HS_HCI_ERR(error_code);
  }

  prv_finish(p, status);
}

bool aci_gattc_rx_vs(uint16_t code, const uint8_t *d, size_t n) {
  struct proc *p;

  if (code == ACI_VS_GATT_NOTIFICATION || code == ACI_VS_GATT_INDICATION) {
    if (n >= 5U) {
      prv_rx_notify(d, n, code == ACI_VS_GATT_INDICATION);
    }
    return true;
  }

  switch (code) {
    case ACI_VS_ATT_FIND_BY_TYPE:
    case ACI_VS_ATT_READ_BY_TYPE:
    case ACI_VS_ATT_FIND_INFO_RESP:
    case ACI_VS_ATT_READ_RESP:
    case ACI_VS_ATT_READ_BLOB_RESP:
    case ACI_VS_GATT_READ_BY_UUID:
    case ACI_VS_GATT_ERROR_RESP:
    case ACI_VS_GATT_PROC_COMPLETE:
    case ACI_VS_GATT_PROC_TIMEOUT:
      break;
    default:
      return false;
  }

  if (n < 3U) {
    return true;
  }

  aci_lock();
  p = prv_active(aci_get_le16(d));
  aci_unlock();
  if (p == nullptr) {
    return true;
  }

  switch (code) {
    case ACI_VS_ATT_FIND_BY_TYPE:
      if (p->type == PROC_DISC_SVC_UUID) {
        prv_rx_find_by_type(p, d, n);
      }
      break;
    case ACI_VS_ATT_READ_BY_TYPE:
      if (n >= 4U && (p->type == PROC_DISC_ALL_CHRS || p->type == PROC_DISC_CHRS_UUID)) {
        prv_rx_read_by_type(p, d, n);
      }
      break;
    case ACI_VS_ATT_FIND_INFO_RESP:
      if (n >= 4U && p->type == PROC_DISC_ALL_DSCS) {
        prv_rx_find_info(p, d, n);
      }
      break;
    case ACI_VS_ATT_READ_RESP:
    case ACI_VS_ATT_READ_BLOB_RESP:
      prv_rx_value(p, &d[3], PBL_MIN((size_t)d[2], n - 3U));
      break;
    case ACI_VS_GATT_READ_BY_UUID:
      if (n >= 5U && p->type == PROC_READ_UUID) {
        uint16_t handle = aci_get_le16(&d[2]);

        prv_cb_attr(p, 0, handle, 0U,
                    ble_hs_mbuf_from_flat(&d[5], (uint16_t)PBL_MIN((size_t)d[4], n - 5U)));
      }
      break;
    case ACI_VS_GATT_ERROR_RESP:
      if (n >= 6U) {
        p->att_err = d[5];
        p->handle = p->handle != 0U ? p->handle : aci_get_le16(&d[3]);
      }
      break;
    case ACI_VS_GATT_PROC_COMPLETE:
      prv_rx_complete(p, d[2]);
      break;
    case ACI_VS_GATT_PROC_TIMEOUT:
      prv_finish(p, BLE_HS_ETIMEOUT);
      break;
    default:
      break;
  }

  return true;
}

void aci_gattc_conn_closed(uint16_t conn_handle) {
  while (true) {
    struct proc *p = nullptr;

    aci_lock();
    for (size_t i = 0U; i < MAX_PROCS && p == nullptr; i++) {
      if (s_procs[i].used && s_procs[i].conn_handle == conn_handle) {
        // Not to start the next one
        s_procs[i].started = true;
        p = &s_procs[i];
      }
    }
    aci_unlock();

    if (p == nullptr) {
      return;
    }
    prv_finish(p, BLE_HS_ENOTCONN);
  }
}

void aci_gattc_init(void) {
  memset(s_procs, 0, sizeof(s_procs));
}
