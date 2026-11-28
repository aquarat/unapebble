/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "gatts_map.h"

#include <string.h>

#include <host/ble_att.h>

#define CHR_F_WRITES (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | \
                      BLE_GATT_CHR_F_AUTH_SIGN_WRITE | BLE_GATT_CHR_F_RELIABLE_WRITE | \
                      BLE_GATT_CHR_F_AUX_WRITE)
#define CHR_F_SUBSCRIBE (BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE)

// The Bluetooth Base UUID, least significant octet first, its 32 bits at 12 to 15
static const uint8_t s_base[16] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                   0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

uint8_t aci_map_uuid(const ble_uuid_t *uuid, uint8_t out[16]) {
  switch (uuid->type) {
    case BLE_UUID_TYPE_16: {
      uint16_t v = ((const ble_uuid16_t *)uuid)->value;

      memset(out, 0, 16);
      out[0] = (uint8_t)v;
      out[1] = (uint8_t)(v >> 8);
      return ACI_UUID_16;
    }
    case BLE_UUID_TYPE_32: {
      uint32_t v = ((const ble_uuid32_t *)uuid)->value;

      memcpy(out, s_base, 16);
      out[12] = (uint8_t)v;
      out[13] = (uint8_t)(v >> 8);
      out[14] = (uint8_t)(v >> 16);
      out[15] = (uint8_t)(v >> 24);
      return ACI_UUID_128;
    }
    default:
      memcpy(out, ((const ble_uuid128_t *)uuid)->value, 16);
      return ACI_UUID_128;
  }
}

uint8_t aci_map_chr_props(ble_gatt_chr_flags flags) {
  // The low seven bits are the properties' own; NimBLE's 0x80 is a reliable write, which the
  // extended properties announce
  uint8_t props = (uint8_t)(flags & 0x7fU);

  if ((flags & (BLE_GATT_CHR_F_RELIABLE_WRITE | BLE_GATT_CHR_F_AUX_WRITE)) != 0U) {
    props |= ACI_PROP_EXT;
  }

  return props;
}

uint8_t aci_map_chr_perms(ble_gatt_chr_flags flags) {
  uint8_t perms = 0U;

  if ((flags & BLE_GATT_CHR_F_READ_ENC) != 0U) {
    perms |= ACI_PERM_ENCRY_READ;
  }
  if ((flags & BLE_GATT_CHR_F_READ_AUTHEN) != 0U) {
    perms |= ACI_PERM_AUTHEN_READ | ACI_PERM_ENCRY_READ;
  }
  if ((flags & BLE_GATT_CHR_F_READ_AUTHOR) != 0U) {
    perms |= ACI_PERM_AUTHOR_READ;
  }
  if ((flags & BLE_GATT_CHR_F_WRITE_ENC) != 0U) {
    perms |= ACI_PERM_ENCRY_WRITE;
  }
  if ((flags & BLE_GATT_CHR_F_WRITE_AUTHEN) != 0U) {
    perms |= ACI_PERM_AUTHEN_WRITE | ACI_PERM_ENCRY_WRITE;
  }
  if ((flags & BLE_GATT_CHR_F_WRITE_AUTHOR) != 0U) {
    perms |= ACI_PERM_AUTHOR_WRITE;
  }

  return perms;
}

uint8_t aci_map_chr_evt_mask(ble_gatt_chr_flags flags) {
  uint8_t mask = 0U;

  if ((flags & BLE_GATT_CHR_F_READ) != 0U) {
    mask |= ACI_GATT_NOTIFY_READ_REQ_WAIT;
  }
  if ((flags & CHR_F_WRITES) == BLE_GATT_CHR_F_WRITE_NO_RSP) {
    mask |= ACI_GATT_NOTIFY_ATTRIBUTE_WRITE;
  } else if ((flags & CHR_F_WRITES) != 0U) {
    mask |= ACI_GATT_NOTIFY_WRITE_REQ_WAIT;
  }
  if ((flags & CHR_F_SUBSCRIBE) != 0U) {
    mask |= ACI_GATT_NOTIFY_ATTRIBUTE_WRITE;
  }

  return mask;
}

uint16_t aci_map_chr_max_len(ble_gatt_chr_flags flags, int cur_len) {
  if ((flags & BLE_GATT_CHR_F_READ) == 0U && (flags & (CHR_F_WRITES | CHR_F_SUBSCRIBE)) != 0U) {
    return ACI_CHR_VALUE_MAX;
  }

  if (cur_len > (int)ACI_CHR_VALUE_MAX) {
    return ACI_CHR_VALUE_MAX;
  }

  return cur_len > (int)ACI_CHR_STATE_MIN ? (uint16_t)cur_len : ACI_CHR_STATE_MIN;
}

uint8_t aci_map_dsc_perms(uint8_t att_flags) {
  uint8_t perms = 0U;

  if ((att_flags & BLE_ATT_F_READ_ENC) != 0U) {
    perms |= ACI_PERM_ENCRY_READ;
  }
  if ((att_flags & BLE_ATT_F_READ_AUTHEN) != 0U) {
    perms |= ACI_PERM_AUTHEN_READ | ACI_PERM_ENCRY_READ;
  }
  if ((att_flags & BLE_ATT_F_READ_AUTHOR) != 0U) {
    perms |= ACI_PERM_AUTHOR_READ;
  }
  if ((att_flags & BLE_ATT_F_WRITE_ENC) != 0U) {
    perms |= ACI_PERM_ENCRY_WRITE;
  }
  if ((att_flags & BLE_ATT_F_WRITE_AUTHEN) != 0U) {
    perms |= ACI_PERM_AUTHEN_WRITE | ACI_PERM_ENCRY_WRITE;
  }
  if ((att_flags & BLE_ATT_F_WRITE_AUTHOR) != 0U) {
    perms |= ACI_PERM_AUTHOR_WRITE;
  }

  return perms;
}

uint8_t aci_map_dsc_access(uint8_t att_flags) {
  uint8_t access = 0U;

  if ((att_flags & BLE_ATT_F_READ) != 0U) {
    access |= ACI_ACCESS_READ;
  }
  if ((att_flags & BLE_ATT_F_WRITE) != 0U) {
    access |= ACI_ACCESS_WRITE;
  }

  return access;
}

uint8_t aci_map_dsc_evt_mask(uint8_t att_flags) {
  uint8_t mask = 0U;

  if ((att_flags & BLE_ATT_F_READ) != 0U) {
    mask |= ACI_GATT_NOTIFY_READ_REQ_WAIT;
  }
  if ((att_flags & BLE_ATT_F_WRITE) != 0U) {
    mask |= ACI_GATT_NOTIFY_WRITE_REQ_WAIT;
  }

  return mask;
}

unsigned int aci_svc_num_attrs(const struct ble_gatt_svc_def *svc) {
  // The service's declaration
  unsigned int n = 1U;

  for (const struct ble_gatt_chr_def *chr = svc->characteristics; chr != nullptr && chr->uuid;
       chr++) {
    // Its declaration and its value
    n += 2U;
    if ((chr->flags & CHR_F_SUBSCRIBE) != 0U) {
      n++;
    }
    if ((aci_map_chr_props(chr->flags) & ACI_PROP_EXT) != 0U) {
      n++;
    }
    for (const struct ble_gatt_dsc_def *dsc = chr->descriptors; dsc != nullptr && dsc->uuid;
         dsc++) {
      n++;
    }
  }

  return n;
}

void aci_svcs_count(const struct ble_gatt_svc_def *svcs, size_t *num_chrs, size_t *num_dscs) {
  *num_chrs = 0U;
  *num_dscs = 0U;

  for (const struct ble_gatt_svc_def *svc = svcs; svc->type != BLE_GATT_SVC_TYPE_END; svc++) {
    for (const struct ble_gatt_chr_def *chr = svc->characteristics; chr != nullptr && chr->uuid;
         chr++) {
      (*num_chrs)++;
      for (const struct ble_gatt_dsc_def *dsc = chr->descriptors; dsc != nullptr && dsc->uuid;
           dsc++) {
        (*num_dscs)++;
      }
    }
  }
}

int aci_table_add_chr(struct aci_attr_table *t, const struct ble_gatt_svc_def *svc,
                      const struct ble_gatt_chr_def *chr, uint16_t svc_handle,
                      uint16_t decl_handle, uint16_t max_len) {
  struct aci_chr *c;

  if (t->num_chrs >= t->max_chrs) {
    return -1;
  }

  c = &t->chrs[t->num_chrs];
  *c = (struct aci_chr){
    .svc = svc,
    .chr = chr,
    .svc_handle = svc_handle,
    .decl_handle = decl_handle,
    .val_handle = decl_handle + 1U,
    .cccd_handle = (chr->flags & CHR_F_SUBSCRIBE) != 0U ? decl_handle + 2U : 0U,
    .evt_mask = aci_map_chr_evt_mask(chr->flags),
    .max_len = max_len,
  };
  c->end_handle = c->cccd_handle != 0U ? c->cccd_handle : c->val_handle;
  if ((aci_map_chr_props(chr->flags) & ACI_PROP_EXT) != 0U) {
    c->end_handle++;
  }

  return (int)t->num_chrs++;
}

int aci_table_add_dsc(struct aci_attr_table *t, uint16_t chr_idx,
                      const struct ble_gatt_dsc_def *dsc, uint16_t handle) {
  if (t->num_dscs >= t->max_dscs || chr_idx >= t->num_chrs) {
    return -1;
  }

  t->dscs[t->num_dscs++] = (struct aci_dsc){.chr_idx = chr_idx, .dsc = dsc, .handle = handle};
  if (handle > t->chrs[chr_idx].end_handle) {
    t->chrs[chr_idx].end_handle = handle;
  }

  return 0;
}

enum aci_attr_kind aci_table_find(const struct aci_attr_table *t, uint16_t handle, size_t *chr_idx,
                                  size_t *dsc_idx) {
  if (handle == 0U) {
    return ACI_ATTR_NONE;
  }

  for (size_t i = 0U; i < t->num_chrs; i++) {
    if (t->chrs[i].val_handle == handle) {
      *chr_idx = i;
      return ACI_ATTR_VALUE;
    }
    if (t->chrs[i].cccd_handle == handle) {
      *chr_idx = i;
      return ACI_ATTR_CCCD;
    }
  }

  for (size_t i = 0U; i < t->num_dscs; i++) {
    if (t->dscs[i].handle == handle) {
      *chr_idx = t->dscs[i].chr_idx;
      *dsc_idx = i;
      return ACI_ATTR_DSC;
    }
  }

  return ACI_ATTR_NONE;
}

int aci_table_find_val(const struct aci_attr_table *t, uint16_t val_handle) {
  for (size_t i = 0U; i < t->num_chrs; i++) {
    if (t->chrs[i].val_handle == val_handle) {
      return (int)i;
    }
  }

  return -1;
}
