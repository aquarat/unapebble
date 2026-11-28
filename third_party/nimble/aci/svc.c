/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The GAP and GATT services: the chip has its own, made by aci_gap_init and aci_gatt_init, which
 * stand in for NimBLE's.
 */

#include "aci_priv.h"

#include <pbl/logging/logging.h>
#include <pbl/util/math.h>

#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

PBL_LOG_MODULE_DECLARE(nimble_aci, CONFIG_NIMBLE_ACI_BLUENRG2_LOG_LEVEL);

#define NAME_MAX 32U

// Where aci_gatt_init puts the GATT service and its Service Changed characteristic
#define GATT_SVC_HANDLE            0x0001U
#define GATT_SERVICE_CHANGED_DECL  0x0002U

static char s_name[NAME_MAX + 1U];
static uint16_t s_appearance;

static int prv_push_name(void) {
  uint8_t buf[6U + NAME_MAX];
  uint16_t svc;
  uint16_t chr;
  uint8_t len = (uint8_t)strlen(s_name);
  uint8_t *p;

  aci_gap_name_handles(&svc, &chr);
  if (svc == 0U) {
    return 0;
  }

  p = aci_put_le16(buf, svc);
  p = aci_put_le16(p, chr);
  *p++ = 0U;
  *p++ = len;
  memcpy(p, s_name, len);
  p += len;

  return aci_cmd(ACI_OP_GATT_UPDATE_CHAR_VALUE, buf, (uint8_t)(p - buf), nullptr, nullptr);
}

int aci_svc_setup(void) {
  return prv_push_name();
}

void ble_svc_gap_init(void) {
}

const char *ble_svc_gap_device_name(void) {
  return s_name;
}

int ble_svc_gap_device_name_set(const char *name) {
  size_t len = strlen(name);

  if (len > NAME_MAX) {
    return BLE_HS_EINVAL;
  }

  aci_lock();
  memcpy(s_name, name, len + 1U);
  aci_unlock();

  if (ble_hs_synced()) {
    return aci_hs_err(prv_push_name());
  }

  return 0;
}

uint16_t ble_svc_gap_device_appearance(void) {
  return s_appearance;
}

int ble_svc_gap_device_appearance_set(uint16_t appearance) {
  s_appearance = appearance;

  return 0;
}

void ble_svc_gap_set_chr_changed_cb(ble_svc_gap_chr_changed_fn *cb) {
}

void ble_svc_gatt_init(void) {
}

uint8_t ble_svc_gatt_get_local_cl_supported_feat(void) {
  return 0U;
}

void ble_svc_gatt_changed(uint16_t start_handle, uint16_t end_handle) {
  uint8_t buf[16];
  uint8_t *p;
  int rc;

  // To every client that subscribed
  p = aci_put_le16(buf, 0U);
  p = aci_put_le16(p, GATT_SVC_HANDLE);
  p = aci_put_le16(p, GATT_SERVICE_CHANGED_DECL);
  // An indication
  *p++ = 0x02U;
  p = aci_put_le16(p, 4U);
  p = aci_put_le16(p, 0U);
  *p++ = 4U;
  p = aci_put_le16(p, start_handle);
  p = aci_put_le16(p, end_handle);

  rc = aci_cmd(ACI_OP_GATT_UPDATE_CHAR_EXT, buf, (uint8_t)(p - buf), nullptr, nullptr);
  if (rc != 0) {
    PBL_LOG_WRN("Service Changed not indicated (%d)", rc);
  }
}
