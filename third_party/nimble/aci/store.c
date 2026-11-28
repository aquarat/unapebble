/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Bonds: the chip keeps them, keys and all, and gives none of their keys out. The host's store
 * keeps a record per bond, without keys, made when a pairing bonds, so that PebbleOS sees its
 * bonds as with NimBLE. Deleting a peer's record deletes the chip's bond too; at each start the
 * two lists are made to agree.
 */

#include "aci_priv.h"

#include <pbl/bluetooth/types.h>
#include <pbl/logging/logging.h>

#include <host/ble_store.h>

PBL_LOG_MODULE_DECLARE(nimble_aci, CONFIG_NIMBLE_ACI_BLUENRG2_LOG_LEVEL);

#define MAX_BONDS      MYNEWT_VAL(BLE_STORE_MAX_BONDS)
#define CHIP_MAX_BONDS 12U

static ble_store_delete_fn *s_inner_delete;

static void prv_chip_remove(const ble_addr_t *addr) {
  uint8_t par[7];
  [[maybe_unused]] int rc;

  par[0] = addr->type == BLE_ADDR_PUBLIC ? 0x00U : 0x01U;
  memcpy(&par[1], addr->val, 6U);
  rc = aci_cmd(ACI_OP_GAP_REMOVE_BONDED, par, sizeof(par), nullptr, nullptr);
  PBL_LOG_DBG("Bond with " PBL_BT_ADDR_FMT " removed from the chip: %d",
              PBL_BT_ADDR_XPLODE_VAL(addr->val), rc);
}

// The store's delete, the chip's bond with it
static int prv_store_delete(int obj_type, const union ble_store_key *key) {
  if (obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC && ble_hs_cfg.store_read_cb != nullptr) {
    union ble_store_value value;

    if (ble_hs_cfg.store_read_cb(obj_type, key, &value) == 0) {
      prv_chip_remove(&value.sec.peer_addr);
    }
  }

  return s_inner_delete != nullptr ? s_inner_delete(obj_type, key) : BLE_HS_ENOENT;
}

static void prv_hook(void) {
  if (ble_hs_cfg.store_delete_cb != prv_store_delete) {
    s_inner_delete = ble_hs_cfg.store_delete_cb;
    ble_hs_cfg.store_delete_cb = prv_store_delete;
  }
}

void aci_store_bonded(const ble_addr_t *peer, bool authenticated) {
  struct ble_store_value_sec value = {
    .peer_addr = *peer,
    .key_size = 16,
    .ltk_present = 1,
    .authenticated = authenticated,
    .sc = 1,
  };
  int rc;

  prv_hook();

  // Ours first: the peer's makes the bond, for the store's users
  rc = ble_store_write_our_sec(&value);
  if (rc == 0) {
    rc = ble_store_write_peer_sec(&value);
  }
  if (rc != 0) {
    PBL_LOG_ERR("Bond not stored (%d)", rc);
  }
}

void aci_store_reconcile(void) {
  uint8_t list[1U + CHIP_MAX_BONDS * 7U];
  size_t len = sizeof(list);
  ble_addr_t peers[MAX_BONDS];
  int num_peers = 0;
  size_t num_chip = 0U;
  int rc;

  prv_hook();

  rc = aci_cmd(ACI_OP_GAP_GET_BONDED_DEVICES, nullptr, 0U, list, &len);
  if (rc != 0 || len < 1U) {
    PBL_LOG_ERR("Bonds of the chip not read (%d)", rc);
    return;
  }
  num_chip = list[0];
  if (1U + num_chip * 7U > len) {
    num_chip = (len - 1U) / 7U;
  }

  (void)ble_store_util_bonded_peers(peers, &num_peers, MAX_BONDS);

  // The store's bonds the chip does not have are of no use
  for (int i = 0; i < num_peers; i++) {
    bool found = false;

    for (size_t j = 0U; j < num_chip && !found; j++) {
      const uint8_t *e = &list[1U + j * 7U];

      found = e[0] == (peers[i].type == BLE_ADDR_PUBLIC ? 0x00U : 0x01U) &&
              memcmp(&e[1], peers[i].val, 6U) == 0;
    }
    if (!found) {
      PBL_LOG_WRN("Bond with " PBL_BT_ADDR_FMT " not on the chip: deleted",
                  PBL_BT_ADDR_XPLODE_VAL(peers[i].val));
      (void)ble_store_util_delete_peer(&peers[i]);
    }
  }

  // The chip's bonds the host has forgotten
  for (size_t j = 0U; j < num_chip; j++) {
    const uint8_t *e = &list[1U + j * 7U];
    ble_addr_t addr = {.type = e[0] == 0x00U ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM};
    bool found = false;

    memcpy(addr.val, &e[1], 6U);
    for (int i = 0; i < num_peers && !found; i++) {
      found = ble_addr_cmp(&addr, &peers[i]) == 0;
    }
    if (!found) {
      prv_chip_remove(&addr);
    }
  }

  PBL_LOG_DBG("Bonds: %u on the chip, %d in the store", (unsigned int)num_chip, num_peers);
}

int ble_gap_unpair_oldest_peer(void) {
  ble_addr_t peers[MAX_BONDS];
  int num = 0;
  int rc;

  rc = ble_store_util_bonded_peers(peers, &num, MAX_BONDS);
  if (rc != 0) {
    return rc;
  }
  if (num == 0) {
    return BLE_HS_ENOENT;
  }

  return ble_gap_unpair(&peers[0]);
}

int ble_gap_unpair_oldest_except(const ble_addr_t *peer_addr) {
  ble_addr_t peers[MAX_BONDS];
  int num = 0;
  int rc;

  rc = ble_store_util_bonded_peers(peers, &num, MAX_BONDS);
  if (rc != 0) {
    return rc;
  }

  for (int i = 0; i < num; i++) {
    if (ble_addr_cmp(&peers[i], peer_addr) != 0) {
      return ble_gap_unpair(&peers[i]);
    }
  }

  return BLE_HS_ENOENT;
}
