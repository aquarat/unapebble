/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/*
 * How NimBLE's GATT service definitions become the chip's: the parameters of the ACI commands
 * that add them, and the table that maps the handles the chip gives back to the definitions.
 * Independent of the chip, for the unit tests.
 */

#include <stddef.h>
#include <stdint.h>

#include <host/ble_gatt.h>
#include <host/ble_uuid.h>

/* The ACI's UUID types */
#define ACI_UUID_16  0x01U
#define ACI_UUID_128 0x02U

/* aci_gatt_add_char's security permissions */
#define ACI_PERM_AUTHEN_READ  0x01U
#define ACI_PERM_AUTHOR_READ  0x02U
#define ACI_PERM_ENCRY_READ   0x04U
#define ACI_PERM_AUTHEN_WRITE 0x08U
#define ACI_PERM_AUTHOR_WRITE 0x10U
#define ACI_PERM_ENCRY_WRITE  0x20U

/* aci_gatt_add_char_desc's access permissions */
#define ACI_ACCESS_READ  0x01U
#define ACI_ACCESS_WRITE 0x02U

/* The GATT event masks of a characteristic or descriptor */
#define ACI_GATT_NOTIFY_ATTRIBUTE_WRITE 0x01U
#define ACI_GATT_NOTIFY_WRITE_REQ_WAIT  0x02U
#define ACI_GATT_NOTIFY_READ_REQ_WAIT   0x04U

/* The characteristic properties' extended properties bit */
#define ACI_PROP_EXT 0x80U

/* The longest value the chip keeps for a characteristic: the most its ATT MTU, 220, carries */
#define ACI_CHR_VALUE_MAX 217U
/* The least kept for one that is read: what a read at the default MTU of 23 gives */
#define ACI_CHR_STATE_MIN 20U
/* For a descriptor */
#define ACI_DSC_VALUE_MAX 32U

/** What an attribute of the chip is to the host. */
enum aci_attr_kind {
  ACI_ATTR_NONE,
  ACI_ATTR_VALUE,
  ACI_ATTR_CCCD,
  ACI_ATTR_DSC,
};

/** A characteristic as added to the chip. */
struct aci_chr {
  const struct ble_gatt_svc_def *svc;
  const struct ble_gatt_chr_def *chr;
  uint16_t svc_handle;
  /* The chip's handle of the characteristic: its declaration */
  uint16_t decl_handle;
  uint16_t val_handle;
  /* Its Client Characteristic Configuration descriptor, or 0 */
  uint16_t cccd_handle;
  /* The last handle of the characteristic, its descriptors included */
  uint16_t end_handle;
  /* ACI_GATT_NOTIFY_*: how its reads and writes reach the host */
  uint8_t evt_mask;
  uint16_t max_len;
};

/** A descriptor as added to the chip. */
struct aci_dsc {
  /* Its characteristic, in the table */
  uint16_t chr_idx;
  const struct ble_gatt_dsc_def *dsc;
  uint16_t handle;
};

/** The chip's attributes, of every service added. */
struct aci_attr_table {
  struct aci_chr *chrs;
  size_t num_chrs;
  size_t max_chrs;
  struct aci_dsc *dscs;
  size_t num_dscs;
  size_t max_dscs;
};

/**
 * A UUID as the ACI takes it, least significant octet first: its type, ACI_UUID_16 or
 * ACI_UUID_128, the bytes in out. A 32-bit one is given as 128 bits.
 */
uint8_t aci_map_uuid(const ble_uuid_t *uuid, uint8_t out[16]);

/** The characteristic properties of a characteristic's flags. */
uint8_t aci_map_chr_props(ble_gatt_chr_flags flags);

/** Its security permissions. */
uint8_t aci_map_chr_perms(ble_gatt_chr_flags flags);

/**
 * Its GATT event mask. Reads are answered by the host. A write that can be refused, a write
 * request, waits for the host; one of the writes without response alone is only reported, which
 * saves the chip and the host a command each. A characteristic that notifies or indicates reports
 * its CCCD's writes.
 */
uint8_t aci_map_chr_evt_mask(ble_gatt_chr_flags flags);

/**
 * The longest value the chip keeps for it, from the memory of a little over a kilobyte the chip
 * has for every value. One that can be read is state: its value now, cur_len, or what one read at
 * the default MTU gives, whichever is longer, if it is not known (cur_len < 0). One that is only
 * written or notified carries data, as much as the chip's MTU takes.
 */
uint16_t aci_map_chr_max_len(ble_gatt_chr_flags flags, int cur_len);

/** A descriptor's security permissions, access permissions and event mask, from its flags. */
uint8_t aci_map_dsc_perms(uint8_t att_flags);
uint8_t aci_map_dsc_access(uint8_t att_flags);
uint8_t aci_map_dsc_evt_mask(uint8_t att_flags);

/** The attribute records a service takes in the chip: aci_gatt_add_service's maximum. */
unsigned int aci_svc_num_attrs(const struct ble_gatt_svc_def *svc);

/** The characteristics and descriptors a NULL-terminated array of services has. */
void aci_svcs_count(const struct ble_gatt_svc_def *svcs, size_t *num_chrs, size_t *num_dscs);

/**
 * Adds a characteristic, from the chip's handle of its declaration. Its value comes next, then
 * its CCCD if it has one. Its index, or -1 if the table is full.
 */
int aci_table_add_chr(struct aci_attr_table *t, const struct ble_gatt_svc_def *svc,
                      const struct ble_gatt_chr_def *chr, uint16_t svc_handle,
                      uint16_t decl_handle, uint16_t max_len);

/** Adds a descriptor of the characteristic chr_idx. 0, or -1 if the table is full. */
int aci_table_add_dsc(struct aci_attr_table *t, uint16_t chr_idx,
                      const struct ble_gatt_dsc_def *dsc, uint16_t handle);

/**
 * What a handle of the chip is: the characteristic it belongs to in *chr_idx, the descriptor in
 * *dsc_idx for ACI_ATTR_DSC.
 */
enum aci_attr_kind aci_table_find(const struct aci_attr_table *t, uint16_t handle, size_t *chr_idx,
                                  size_t *dsc_idx);

/** The characteristic of a value handle, or -1. */
int aci_table_find_val(const struct aci_attr_table *t, uint16_t val_handle);
