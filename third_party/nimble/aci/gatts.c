/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The GATT server: NimBLE's service definitions, added to the chip at each of its starts, the
 * handles it gives written to their val_handle, and its read and write requests handed to their
 * access callbacks (gatts_map.c).
 */

#include "aci_priv.h"
#include "gatts_map.h"

#include <pbl/logging/logging.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#include <host/ble_hs_mbuf.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>

PBL_LOG_MODULE_DECLARE(nimble_aci, CONFIG_NIMBLE_ACI_BLUENRG2_LOG_LEVEL);

#define MAX_SVC_DEFS CONFIG_NIMBLE_ACI_MAX_SVC_DEFS
#define MAX_CHRS     CONFIG_NIMBLE_ACI_MAX_CHRS
#define MAX_DSCS     CONFIG_NIMBLE_ACI_MAX_DSCS
#define MAX_CONNS    MYNEWT_VAL(BLE_MAX_CONNECTIONS)

// A characteristic value update's parameters, but for the value
#define UPDATE_HDR_LEN 12U
#define UPDATE_CHUNK   (ACI_MAX_PARAMS - UPDATE_HDR_LEN)

#define UPDATE_LOCAL    0x00U
#define UPDATE_NOTIFY   0x01U
#define UPDATE_INDICATE 0x02U

#define CCCD_UUID16   0x2902U
#define CCCD_NOTIFY   0x0001U
#define CCCD_INDICATE 0x0002U

struct sub {
  uint16_t conn_handle;
  uint8_t flags;
};

static const struct ble_gatt_svc_def *s_defs[MAX_SVC_DEFS];
static size_t s_num_defs;
static bool s_started;

static struct aci_chr s_chrs[MAX_CHRS];
static struct aci_dsc s_dscs[MAX_DSCS];
static struct aci_attr_table s_table = {
  .chrs = s_chrs,
  .max_chrs = MAX_CHRS,
  .dscs = s_dscs,
  .max_dscs = MAX_DSCS,
};
static struct sub s_subs[MAX_CHRS][MAX_CONNS];

// A value written in parts (the attribute-modified event's offset bit 15)
static uint8_t s_part[ACI_CHR_VALUE_MAX];
static uint16_t s_part_len;
static uint16_t s_part_handle;

// --- Registration ------------------------------------------------------------------------------

int ble_gatts_count_cfg(const struct ble_gatt_svc_def *defs) {
  size_t num_chrs;
  size_t num_dscs;

  aci_svcs_count(defs, &num_chrs, &num_dscs);

  return 0;
}

int ble_gatts_add_svcs(const struct ble_gatt_svc_def *svcs) {
  int rc = 0;

  aci_lock();
  if (s_started) {
    rc = BLE_HS_EBUSY;
  } else if (s_num_defs >= MAX_SVC_DEFS) {
    rc = BLE_HS_ENOMEM;
  } else {
    s_defs[s_num_defs++] = svcs;
  }
  aci_unlock();

  return rc;
}

int ble_gatts_reset(void) {
  int rc = 0;

  aci_lock();
  if (ble_hs_synced()) {
    rc = BLE_HS_EBUSY;
  } else {
    s_num_defs = 0U;
    s_table.num_chrs = 0U;
    s_table.num_dscs = 0U;
    s_started = false;
  }
  aci_unlock();

  return rc;
}

static int prv_add_svc(const struct ble_gatt_svc_def *svc, uint16_t *svc_handle) {
  uint8_t buf[ACI_MAX_PARAMS];
  uint8_t *p = buf;
  uint8_t rsp[2];
  size_t len = sizeof(rsp);
  uint8_t type = aci_map_uuid(svc->uuid, &p[1]);
  int rc;

  p[0] = type;
  p += 1U + (type == ACI_UUID_16 ? 2U : 16U);
  *p++ = svc->type == BLE_GATT_SVC_TYPE_PRIMARY ? 0x01U : 0x02U;
  *p++ = (uint8_t)PBL_MIN(aci_svc_num_attrs(svc), 255U);

  rc = aci_cmd(ACI_OP_GATT_ADD_SERVICE, buf, (uint8_t)(p - buf), rsp, &len);
  if (rc == 0 && len >= 2U) {
    *svc_handle = aci_get_le16(rsp);
  }

  return rc;
}

static int prv_read_cb(uint16_t conn_handle, const struct aci_chr *c, const struct aci_dsc *d,
                       uint8_t *val, uint16_t max, uint16_t *len);

// The length of a characteristic's value now, read without a connection, or -1
static int prv_cur_len(const struct ble_gatt_svc_def *svc, const struct ble_gatt_chr_def *chr) {
  const struct aci_chr c = {.svc = svc, .chr = chr};
  uint8_t value[ACI_CHR_VALUE_MAX];
  uint16_t len;

  if ((chr->flags & BLE_GATT_CHR_F_READ) == 0U ||
      prv_read_cb(BLE_HS_CONN_HANDLE_NONE, &c, nullptr, value, sizeof(value), &len) != 0) {
    return -1;
  }

  return len;
}

static int prv_add_chr(const struct ble_gatt_chr_def *chr, uint16_t svc_handle, uint16_t max_len,
                       uint16_t *decl_handle) {
  uint8_t buf[ACI_MAX_PARAMS];
  uint8_t *p = aci_put_le16(buf, svc_handle);
  uint8_t rsp[2];
  size_t len = sizeof(rsp);
  uint8_t type = aci_map_uuid(chr->uuid, &p[1]);
  int rc;

  p[0] = type;
  p += 1U + (type == ACI_UUID_16 ? 2U : 16U);
  p = aci_put_le16(p, max_len);
  *p++ = aci_map_chr_props(chr->flags);
  *p++ = aci_map_chr_perms(chr->flags);
  *p++ = aci_map_chr_evt_mask(chr->flags);
  *p++ = (uint8_t)PBL_CLAMP(chr->min_key_size, 7U, 16U);
  // Variable length
  *p++ = 1U;

  rc = aci_cmd(ACI_OP_GATT_ADD_CHAR, buf, (uint8_t)(p - buf), rsp, &len);
  if (rc == 0 && len >= 2U) {
    *decl_handle = aci_get_le16(rsp);
  }

  return rc;
}

static int prv_add_dsc(const struct ble_gatt_dsc_def *dsc, uint16_t svc_handle,
                       uint16_t decl_handle, uint16_t *handle) {
  uint8_t buf[ACI_MAX_PARAMS];
  uint8_t *p = aci_put_le16(buf, svc_handle);
  uint8_t rsp[2];
  size_t len = sizeof(rsp);
  uint8_t type;
  int rc;

  p = aci_put_le16(p, decl_handle);
  type = aci_map_uuid(dsc->uuid, &p[1]);
  p[0] = type;
  p += 1U + (type == ACI_UUID_16 ? 2U : 16U);
  *p++ = ACI_DSC_VALUE_MAX;
  // Empty until read
  *p++ = 0U;
  *p++ = aci_map_dsc_perms(dsc->att_flags);
  *p++ = aci_map_dsc_access(dsc->att_flags);
  *p++ = aci_map_dsc_evt_mask(dsc->att_flags);
  *p++ = (uint8_t)PBL_CLAMP(dsc->min_key_size, 7U, 16U);
  *p++ = 1U;

  rc = aci_cmd(ACI_OP_GATT_ADD_CHAR_DESC, buf, (uint8_t)(p - buf), rsp, &len);
  if (rc == 0 && len >= 2U) {
    *handle = aci_get_le16(rsp);
  }

  return rc;
}

static int prv_add_defs(const struct ble_gatt_svc_def *svcs) {
  int rc;

  for (const struct ble_gatt_svc_def *svc = svcs; svc->type != BLE_GATT_SVC_TYPE_END; svc++) {
    uint16_t svc_handle = 0U;

    rc = prv_add_svc(svc, &svc_handle);
    if (rc != 0) {
      PBL_LOG_ERR("Service not added (%d)", rc);
      return rc;
    }

    for (const struct ble_gatt_chr_def *chr = svc->characteristics; chr != nullptr && chr->uuid;
         chr++) {
      uint16_t max_len = aci_map_chr_max_len(chr->flags, prv_cur_len(svc, chr));
      uint16_t decl = 0U;
      int idx;

      rc = prv_add_chr(chr, svc_handle, max_len, &decl);
      if (rc != 0) {
        PBL_LOG_ERR("Characteristic not added (%d)", rc);
        return rc;
      }

      aci_lock();
      idx = aci_table_add_chr(&s_table, svc, chr, svc_handle, decl, max_len);
      aci_unlock();
      if (idx < 0) {
        PBL_LOG_ERR("Too many characteristics");
        return BLE_HS_ENOMEM;
      }
      if (chr->val_handle != nullptr) {
        *chr->val_handle = s_chrs[idx].val_handle;
      }
      PBL_LOG_DBG("Characteristic at %04x, %u bytes, events %02x", s_chrs[idx].val_handle, max_len,
                  s_chrs[idx].evt_mask);

      for (const struct ble_gatt_dsc_def *dsc = chr->descriptors; dsc != nullptr && dsc->uuid;
           dsc++) {
        uint16_t handle = 0U;

        // The chip makes the CCCD itself
        if (ble_uuid_cmp(dsc->uuid, BLE_UUID16_DECLARE(CCCD_UUID16)) == 0) {
          continue;
        }

        rc = prv_add_dsc(dsc, svc_handle, decl, &handle);
        if (rc != 0) {
          PBL_LOG_ERR("Descriptor not added (%d)", rc);
          return rc;
        }

        aci_lock();
        rc = aci_table_add_dsc(&s_table, (uint16_t)idx, dsc, handle);
        aci_unlock();
        if (rc != 0) {
          PBL_LOG_ERR("Too many descriptors");
          return BLE_HS_ENOMEM;
        }
      }
    }

    PBL_LOG_DBG("Service at %04x, %u attributes", svc_handle, aci_svc_num_attrs(svc));
  }

  return 0;
}

int aci_gatts_setup(void) {
  int rc = 0;

  aci_lock();
  s_table.num_chrs = 0U;
  s_table.num_dscs = 0U;
  memset(s_subs, 0, sizeof(s_subs));
  s_started = true;
  aci_unlock();

  for (size_t i = 0U; i < s_num_defs && rc == 0; i++) {
    rc = prv_add_defs(s_defs[i]);
  }

  return rc;
}

void aci_gatts_init(void) {
  s_num_defs = 0U;
}

int ble_gatts_find_chr(const ble_uuid_t *svc_uuid, const ble_uuid_t *chr_uuid,
                       uint16_t *out_def_handle, uint16_t *out_val_handle) {
  int rc = BLE_HS_ENOENT;

  aci_lock();
  for (size_t i = 0U; i < s_table.num_chrs; i++) {
    const struct aci_chr *c = &s_chrs[i];

    if (ble_uuid_cmp(c->svc->uuid, svc_uuid) == 0 && ble_uuid_cmp(c->chr->uuid, chr_uuid) == 0) {
      if (out_def_handle != nullptr) {
        *out_def_handle = c->decl_handle;
      }
      if (out_val_handle != nullptr) {
        *out_val_handle = c->val_handle;
      }
      rc = 0;
      break;
    }
  }
  aci_unlock();

  return rc;
}

int ble_gatts_find_svc(const ble_uuid_t *uuid, uint16_t *out_handle) {
  int rc = BLE_HS_ENOENT;

  aci_lock();
  for (size_t i = 0U; i < s_table.num_chrs; i++) {
    if (ble_uuid_cmp(s_chrs[i].svc->uuid, uuid) == 0) {
      if (out_handle != nullptr) {
        *out_handle = s_chrs[i].svc_handle;
      }
      rc = 0;
      break;
    }
  }
  aci_unlock();

  return rc;
}

// --- Values to the chip ------------------------------------------------------------------------

// The characteristic's value set, notified or indicated with the last part
static int prv_update(uint16_t conn_handle, const struct aci_chr *c, uint8_t type,
                      const uint8_t *val, uint16_t len) {
  uint8_t buf[ACI_MAX_PARAMS];
  uint16_t off = 0U;

  do {
    uint16_t n = PBL_MIN((uint16_t)(len - off), (uint16_t)UPDATE_CHUNK);
    bool last = off + n == len;
    uint8_t *p = aci_put_le16(buf, conn_handle);
    int rc;

    p = aci_put_le16(p, c->svc_handle);
    p = aci_put_le16(p, c->decl_handle);
    *p++ = last ? type : UPDATE_LOCAL;
    p = aci_put_le16(p, len);
    p = aci_put_le16(p, off);
    *p++ = (uint8_t)n;
    memcpy(p, &val[off], n);
    p += n;

    rc = aci_cmd(ACI_OP_GATT_UPDATE_CHAR_EXT, buf, (uint8_t)(p - buf), nullptr, nullptr);
    if (rc != 0) {
      return rc;
    }
    off += n;
  } while (off < len);

  return 0;
}

// The access callback's answer to a read, flat, or an ATT error
static int prv_read_cb(uint16_t conn_handle, const struct aci_chr *c, const struct aci_dsc *d,
                       uint8_t *val, uint16_t max, uint16_t *len) {
  struct ble_gatt_access_ctxt ctxt = {};
  struct os_mbuf *om = os_msys_get_pkthdr(0, 0);
  ble_gatt_access_fn *cb = d != nullptr ? d->dsc->access_cb : c->chr->access_cb;
  void *arg = d != nullptr ? d->dsc->arg : c->chr->arg;
  int rc;

  *len = 0U;
  if (om == nullptr) {
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (cb == nullptr) {
    os_mbuf_free_chain(om);
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
  }

  ctxt.om = om;
  if (d != nullptr) {
    ctxt.op = BLE_GATT_ACCESS_OP_READ_DSC;
    ctxt.dsc = d->dsc;
  } else {
    ctxt.op = BLE_GATT_ACCESS_OP_READ_CHR;
    ctxt.chr = c->chr;
  }

  rc = cb(conn_handle, d != nullptr ? d->handle : c->val_handle, &ctxt, arg);
  if (rc == 0) {
    uint16_t n = (uint16_t)PBL_MIN(OS_MBUF_PKTLEN(ctxt.om), (uint32_t)max);

    if (OS_MBUF_PKTLEN(ctxt.om) > max) {
      PBL_LOG_WRN("Value of %04x cut to %u bytes", c->val_handle, max);
    }
    (void)os_mbuf_copydata(ctxt.om, 0, n, val);
    *len = n;
  }
  os_mbuf_free_chain(ctxt.om);

  return rc;
}

static int prv_write_cb(uint16_t conn_handle, const struct aci_chr *c, const struct aci_dsc *d,
                        const uint8_t *val, uint16_t len) {
  struct ble_gatt_access_ctxt ctxt = {};
  ble_gatt_access_fn *cb = d != nullptr ? d->dsc->access_cb : c->chr->access_cb;
  void *arg = d != nullptr ? d->dsc->arg : c->chr->arg;
  int rc;

  if (cb == nullptr) {
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  }

  ctxt.om = ble_hs_mbuf_from_flat(val, len);
  if (ctxt.om == nullptr) {
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (d != nullptr) {
    ctxt.op = BLE_GATT_ACCESS_OP_WRITE_DSC;
    ctxt.dsc = d->dsc;
  } else {
    ctxt.op = BLE_GATT_ACCESS_OP_WRITE_CHR;
    ctxt.chr = c->chr;
  }

  rc = cb(conn_handle, d != nullptr ? d->handle : c->val_handle, &ctxt, arg);
  os_mbuf_free_chain(ctxt.om);

  return rc;
}

// --- Requests from the chip, on the host's thread -----------------------------------------------

static struct sub *prv_sub(size_t chr_idx, uint16_t conn_handle, bool add) {
  struct sub *free = nullptr;

  for (size_t i = 0U; i < MAX_CONNS; i++) {
    struct sub *s = &s_subs[chr_idx][i];

    if (s->flags != 0U && s->conn_handle == conn_handle) {
      return s;
    }
    if (s->flags == 0U && free == nullptr) {
      free = s;
    }
  }

  if (add && free != nullptr) {
    free->conn_handle = conn_handle;
  }

  return add ? free : nullptr;
}

static void prv_subscribe(uint16_t conn_handle, size_t chr_idx, uint8_t flags, uint8_t reason) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_SUBSCRIBE};
  struct sub *s;
  uint8_t prev;

  aci_lock();
  s = prv_sub(chr_idx, conn_handle, true);
  prev = (s != nullptr && s->conn_handle == conn_handle) ? s->flags : 0U;
  if (s != nullptr) {
    s->flags = flags;
  }
  aci_unlock();

  if (prev == flags) {
    return;
  }

  event.subscribe.conn_handle = conn_handle;
  event.subscribe.attr_handle = s_chrs[chr_idx].val_handle;
  event.subscribe.reason = reason;
  event.subscribe.prev_notify = (prev & CCCD_NOTIFY) != 0U;
  event.subscribe.cur_notify = (flags & CCCD_NOTIFY) != 0U;
  event.subscribe.prev_indicate = (prev & CCCD_INDICATE) != 0U;
  event.subscribe.cur_indicate = (flags & CCCD_INDICATE) != 0U;
  (void)aci_gap_event(&event, conn_handle);
}

static void prv_rx_read_permit(const uint8_t *p, size_t n) {
  uint8_t buf[ACI_MAX_PARAMS];
  uint16_t conn = aci_get_le16(p);
  uint16_t handle = aci_get_le16(&p[2]);
  uint16_t offset = aci_get_le16(&p[4]);
  size_t chr_idx = 0U;
  size_t dsc_idx = 0U;
  enum aci_attr_kind kind;
  uint8_t par[3];
  int rc = 0;

  aci_lock();
  kind = aci_table_find(&s_table, handle, &chr_idx, &dsc_idx);
  aci_unlock();

  // A read blob after the first part: the value is there
  if (offset == 0U && (kind == ACI_ATTR_VALUE || kind == ACI_ATTR_DSC)) {
    uint8_t value[ACI_CHR_VALUE_MAX];
    const struct aci_chr *c = &s_chrs[chr_idx];
    const struct aci_dsc *d = kind == ACI_ATTR_DSC ? &s_dscs[dsc_idx] : nullptr;
    uint16_t len;

    rc = prv_read_cb(conn, c, d, value, d != nullptr ? ACI_DSC_VALUE_MAX : c->max_len, &len);
    if (rc == 0 && d == nullptr) {
      rc = prv_update(conn, c, UPDATE_LOCAL, value, len);
    } else if (rc == 0) {
      uint8_t *q = aci_put_le16(buf, c->svc_handle);

      q = aci_put_le16(q, c->decl_handle);
      q = aci_put_le16(q, d->handle);
      q = aci_put_le16(q, 0U);
      *q++ = (uint8_t)len;
      memcpy(q, value, len);
      q += len;
      rc = aci_cmd(ACI_OP_GATT_SET_DESC_VALUE, buf, (uint8_t)(q - buf), nullptr, nullptr);
    } else {
      PBL_LOG_DBG("Read of %04x refused: %d", handle, rc);
    }
  }

  (void)aci_put_le16(par, conn);
  if (rc > 0 && rc <= 0xff) {
    par[2] = (uint8_t)rc;
    if (aci_cmd(ACI_OP_GATT_DENY_READ, par, 3U, nullptr, nullptr) == 0) {
      return;
    }
  } else if (rc != 0) {
    PBL_LOG_ERR("Value of %04x not set (%d)", handle, rc);
  }
  (void)aci_cmd(ACI_OP_GATT_ALLOW_READ, par, 2U, nullptr, nullptr);
}

static void prv_rx_write_permit(const uint8_t *p, size_t n) {
  uint8_t buf[ACI_MAX_PARAMS];
  uint16_t conn = aci_get_le16(p);
  uint16_t handle = aci_get_le16(&p[2]);
  uint8_t len = PBL_MIN(p[4], (uint8_t)(n - 5U));
  const uint8_t *val = &p[5];
  size_t chr_idx = 0U;
  size_t dsc_idx = 0U;
  enum aci_attr_kind kind;
  uint8_t *q;
  int rc = 0;

  aci_lock();
  kind = aci_table_find(&s_table, handle, &chr_idx, &dsc_idx);
  aci_unlock();

  switch (kind) {
    case ACI_ATTR_VALUE:
      rc = prv_write_cb(conn, &s_chrs[chr_idx], nullptr, val, len);
      break;
    case ACI_ATTR_DSC:
      rc = prv_write_cb(conn, &s_chrs[chr_idx], &s_dscs[dsc_idx], val, len);
      break;
    case ACI_ATTR_CCCD:
      if (len >= 2U) {
        prv_subscribe(conn, chr_idx, (uint8_t)aci_get_le16(val), BLE_GAP_SUBSCRIBE_REASON_WRITE);
      }
      break;
    default:
      break;
  }

  q = aci_put_le16(buf, conn);
  q = aci_put_le16(q, handle);
  *q++ = rc != 0 ? 1U : 0U;
  *q++ = rc > 0 && rc <= 0xff ? (uint8_t)rc : (rc != 0 ? BLE_ATT_ERR_UNLIKELY : 0U);
  *q++ = len;
  memcpy(q, val, len);
  q += len;
  (void)aci_cmd(ACI_OP_GATT_WRITE_RESP, buf, (uint8_t)(q - buf), nullptr, nullptr);
}

static void prv_rx_attr_modified(const uint8_t *p, size_t n) {
  uint16_t conn = aci_get_le16(p);
  uint16_t handle = aci_get_le16(&p[2]);
  uint16_t offset = aci_get_le16(&p[4]);
  uint16_t len = PBL_MIN(aci_get_le16(&p[6]), (uint16_t)(n - 8U));
  const uint8_t *val = &p[8];
  bool more = (offset & 0x8000U) != 0U;
  size_t chr_idx = 0U;
  size_t dsc_idx = 0U;
  enum aci_attr_kind kind;

  offset &= 0x7fffU;

  aci_lock();
  kind = aci_table_find(&s_table, handle, &chr_idx, &dsc_idx);
  aci_unlock();

  if (kind == ACI_ATTR_CCCD) {
    if (len >= 2U) {
      prv_subscribe(conn, chr_idx, (uint8_t)aci_get_le16(val), BLE_GAP_SUBSCRIBE_REASON_WRITE);
    }
    return;
  }

  // Writes that waited for the host have been seen
  if (kind != ACI_ATTR_VALUE || (s_chrs[chr_idx].evt_mask & ACI_GATT_NOTIFY_WRITE_REQ_WAIT) != 0U) {
    return;
  }

  if (more || offset != 0U) {
    if (offset == 0U || handle != s_part_handle) {
      s_part_len = 0U;
      s_part_handle = handle;
    }
    if (offset == s_part_len && offset + len <= sizeof(s_part)) {
      memcpy(&s_part[offset], val, len);
      s_part_len = offset + len;
    }
    if (more) {
      return;
    }
    val = s_part;
    len = s_part_len;
    s_part_handle = 0U;
  }

  (void)prv_write_cb(conn, &s_chrs[chr_idx], nullptr, val, len);
}

static void prv_notify_tx(uint16_t conn_handle, uint16_t val_handle, int status, bool indication) {
  struct ble_gap_event event = {.type = BLE_GAP_EVENT_NOTIFY_TX};

  event.notify_tx.conn_handle = conn_handle;
  event.notify_tx.attr_handle = val_handle;
  event.notify_tx.status = status;
  event.notify_tx.indication = indication;
  (void)aci_gap_event(&event, conn_handle);
}

// The indication waiting for its confirmation, per connection
static uint16_t s_ind_handle;

bool aci_gatts_rx_vs(uint16_t code, const uint8_t *p, size_t n) {
  switch (code) {
    case ACI_VS_GATT_READ_PERMIT:
      if (n >= 6U) {
        prv_rx_read_permit(p, n);
      }
      return true;
    case ACI_VS_GATT_WRITE_PERMIT:
      if (n >= 5U) {
        prv_rx_write_permit(p, n);
      }
      return true;
    case ACI_VS_GATT_ATTR_MODIFIED:
      if (n >= 8U) {
        prv_rx_attr_modified(p, n);
      }
      return true;
    case ACI_VS_GATT_SERVER_CONF:
      if (n >= 2U && s_ind_handle != 0U) {
        prv_notify_tx(aci_get_le16(p), s_ind_handle, BLE_HS_EDONE, true);
        s_ind_handle = 0U;
      }
      return true;
    case ACI_VS_GATT_TX_POOL_AVAIL:
      return true;
    default:
      return false;
  }
}

void aci_gatts_conn_closed(uint16_t conn_handle) {
  for (size_t i = 0U; i < s_table.num_chrs; i++) {
    aci_lock();
    struct sub *s = prv_sub(i, conn_handle, false);
    aci_unlock();

    if (s != nullptr) {
      prv_subscribe(conn_handle, i, 0U, BLE_GAP_SUBSCRIBE_REASON_TERM);
    }
  }
  s_ind_handle = 0U;
}

// --- Notifications and indications -------------------------------------------------------------

static int prv_send(uint16_t conn_handle, uint16_t val_handle, struct os_mbuf *om, uint8_t type) {
  uint8_t value[ACI_CHR_VALUE_MAX];
  struct aci_chr c;
  uint16_t len;
  int idx;
  int rc;

  aci_lock();
  idx = aci_table_find_val(&s_table, val_handle);
  if (idx >= 0) {
    c = s_chrs[idx];
  }
  aci_unlock();

  if (idx < 0) {
    os_mbuf_free_chain(om);
    return BLE_HS_ENOENT;
  }
  if (!aci_gap_conn_exists(conn_handle)) {
    os_mbuf_free_chain(om);
    return BLE_HS_ENOTCONN;
  }

  // As NimBLE: what is past the MTU's room is cut
  len = (uint16_t)PBL_MIN(OS_MBUF_PKTLEN(om),
                          (uint32_t)PBL_MIN(c.max_len, ble_att_mtu(conn_handle) - 3U));
  (void)os_mbuf_copydata(om, 0, len, value);
  os_mbuf_free_chain(om);

  rc = prv_update(conn_handle, &c, type, value, len);
  if (rc == 0 && type == UPDATE_INDICATE) {
    s_ind_handle = val_handle;
  }

  return aci_hs_err(rc);
}

int ble_gatts_notify_custom(uint16_t conn_handle, uint16_t att_handle, struct os_mbuf *om) {
  int rc = prv_send(conn_handle, att_handle, om, UPDATE_NOTIFY);

  if (rc == 0) {
    prv_notify_tx(conn_handle, att_handle, 0, false);
  }

  return rc;
}

int ble_gatts_indicate_custom(uint16_t conn_handle, uint16_t chr_val_handle, struct os_mbuf *om) {
  return prv_send(conn_handle, chr_val_handle, om, UPDATE_INDICATE);
}

// The value the access callback gives, sent
static int prv_send_current(uint16_t conn_handle, uint16_t val_handle, uint8_t type) {
  uint8_t value[ACI_CHR_VALUE_MAX];
  struct aci_chr c;
  uint16_t len;
  int idx;
  int rc;

  aci_lock();
  idx = aci_table_find_val(&s_table, val_handle);
  if (idx >= 0) {
    c = s_chrs[idx];
  }
  aci_unlock();

  if (idx < 0) {
    return BLE_HS_ENOENT;
  }

  rc = prv_read_cb(conn_handle, &c, nullptr, value, c.max_len, &len);
  if (rc != 0) {
    return BLE_HS_EAPP;
  }

  return prv_send(conn_handle, val_handle, ble_hs_mbuf_from_flat(value, len), type);
}

int ble_gatts_notify(uint16_t conn_handle, uint16_t chr_val_handle) {
  return prv_send_current(conn_handle, chr_val_handle, UPDATE_NOTIFY);
}

int ble_gatts_indicate(uint16_t conn_handle, uint16_t chr_val_handle) {
  return prv_send_current(conn_handle, chr_val_handle, UPDATE_INDICATE);
}

void ble_gatts_chr_updated(uint16_t chr_val_handle) {
  struct sub subs[MAX_CONNS];
  int idx;

  aci_lock();
  idx = aci_table_find_val(&s_table, chr_val_handle);
  if (idx >= 0) {
    memcpy(subs, s_subs[idx], sizeof(subs));
  }
  aci_unlock();

  if (idx < 0) {
    return;
  }

  for (size_t i = 0U; i < MAX_CONNS; i++) {
    if ((subs[i].flags & CCCD_NOTIFY) != 0U) {
      (void)prv_send_current(subs[i].conn_handle, chr_val_handle, UPDATE_NOTIFY);
    } else if ((subs[i].flags & CCCD_INDICATE) != 0U) {
      (void)prv_send_current(subs[i].conn_handle, chr_val_handle, UPDATE_INDICATE);
    }
  }
}
