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
#include <pbl/util/math.h>

#include <host/ble_store.h>

PBL_LOG_MODULE_DECLARE(nimble_aci, CONFIG_NIMBLE_ACI_BLUENRG2_LOG_LEVEL);

#define MAX_BONDS      MYNEWT_VAL(BLE_STORE_MAX_BONDS)
#define CHIP_MAX_BONDS 12U

static ble_store_delete_fn *s_inner_delete;

int aci_chip_bonds(ble_addr_t *out, int max) {
  uint8_t list[1U + CHIP_MAX_BONDS * 7U];
  size_t len = sizeof(list);
  int num;

  if (aci_cmd(ACI_OP_GAP_GET_BONDED_DEVICES, nullptr, 0U, list, &len) != 0 || len < 1U) {
    return -1;
  }

  num = (int)PBL_MIN((size_t)list[0], (len - 1U) / 7U);
  num = PBL_MIN(num, max);
  for (int i = 0; i < num; i++) {
    const uint8_t *e = &list[1U + (size_t)i * 7U];

    out[i].type = e[0] == 0x00U ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM;
    memcpy(out[i].val, &e[1], 6U);
  }

  return num;
}

bool aci_chip_bond_match(const ble_addr_t *listed, const ble_addr_t *addr) {
  return ble_addr_cmp(listed, addr) == 0;
}

static bool prv_chip_has(const ble_addr_t *addr) {
  ble_addr_t bonds[CHIP_MAX_BONDS];
  int num = aci_chip_bonds(bonds, CHIP_MAX_BONDS);

  for (int i = 0; i < num; i++) {
    if (aci_chip_bond_match(&bonds[i], addr)) {
      return true;
    }
  }

  return false;
}

static int prv_remove(const ble_addr_t *addr) {
  uint8_t par[7];

  par[0] = addr->type == BLE_ADDR_PUBLIC ? 0x00U : 0x01U;
  memcpy(&par[1], addr->val, 6U);

  return aci_cmd(ACI_OP_GAP_REMOVE_BONDED, par, sizeof(par), nullptr, nullptr);
}

// The chip's bond with a peer removed; all of its bonds if that is the only way and the host keeps
// no other
static void prv_chip_remove(const ble_addr_t *addr, bool others_kept) {
  [[maybe_unused]] int rc = prv_remove(addr);

  if (!prv_chip_has(addr)) {
    PBL_LOG_DBG("Bond with " PBL_BT_ADDR_FMT " removed from the chip", PBL_BT_ADDR_XPLODE_VAL(addr->val));
    return;
  }

  if (others_kept) {
    PBL_LOG_ERR("Bond with " PBL_BT_ADDR_FMT " not removed from the chip (%d)",
                PBL_BT_ADDR_XPLODE_VAL(addr->val), rc);
    return;
  }

  rc = aci_cmd(ACI_OP_GAP_CLEAR_SECURITY_DB, nullptr, 0U, nullptr, nullptr);
  PBL_LOG_WRN("Bond with " PBL_BT_ADDR_FMT " not removed alone: the chip's bonds cleared (%d)",
              PBL_BT_ADDR_XPLODE_VAL(addr->val), rc);
}

static int prv_store_count(void) {
  int count = 0;

  (void)ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);

  return count;
}

// The store's delete, the chip's bond with it
static int prv_store_delete(int obj_type, const union ble_store_key *key) {
  if (obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC && ble_hs_cfg.store_read_cb != nullptr) {
    union ble_store_value value;

    if (ble_hs_cfg.store_read_cb(obj_type, key, &value) == 0) {
      prv_chip_remove(&value.sec.peer_addr, prv_store_count() > 1);
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
  ble_addr_t chip[CHIP_MAX_BONDS];
  ble_addr_t peers[MAX_BONDS];
  int num_peers = 0;
  int num_chip;
  bool unknown = false;

  prv_hook();

  num_chip = aci_chip_bonds(chip, CHIP_MAX_BONDS);
  if (num_chip < 0) {
    PBL_LOG_ERR("Bonds of the chip not read");
    return;
  }

  (void)ble_store_util_bonded_peers(peers, &num_peers, MAX_BONDS);

  // The store's bonds the chip does not have are of no use
  for (int i = 0; i < num_peers; i++) {
    bool found = false;

    for (int j = 0; j < num_chip && !found; j++) {
      found = aci_chip_bond_match(&chip[j], &peers[i]);
    }
    if (!found) {
      PBL_LOG_WRN("Bond with " PBL_BT_ADDR_FMT " not on the chip: deleted",
                  PBL_BT_ADDR_XPLODE_VAL(peers[i].val));
      (void)ble_store_util_delete_peer(&peers[i]);
    }
  }

  // The chip's bonds the host has forgotten, which can only go all together
  for (int j = 0; j < num_chip; j++) {
    bool found = false;

    for (int i = 0; i < num_peers && !found; i++) {
      found = aci_chip_bond_match(&chip[j], &peers[i]);
    }
    unknown = unknown || !found;
  }
  if (unknown && prv_store_count() == 0) {
    PBL_LOG_WRN("Bonds of the chip the host has not: cleared");
    (void)aci_cmd(ACI_OP_GAP_CLEAR_SECURITY_DB, nullptr, 0U, nullptr, nullptr);
  } else if (unknown) {
    PBL_LOG_WRN("Bonds of the chip the host has not: kept");
  }

  PBL_LOG_DBG("Bonds: %d on the chip, %d in the store", num_chip, num_peers);
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
