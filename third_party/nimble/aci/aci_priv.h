/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/*
 * NimBLE's host API over the stack of an ST BlueNRG-2, through ST's ACI (decision
 * 261128-081601-02). The opcodes and event layouts are those of ST's ACI for the BlueNRG-1 and
 * BlueNRG-2 (BLE stack 2.x), as the chip's DTM firmware takes them.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>

// --- Commands ----------------------------------------------------------------------------------

#define ACI_OP_HCI_DISCONNECT          0x0406U
#define ACI_OP_HCI_SET_EVENT_MASK      0x0c01U
#define ACI_OP_HCI_LE_SET_EVENT_MASK   0x2001U
#define ACI_OP_HCI_LE_SET_RANDOM_ADDR  0x2005U
#define ACI_OP_HCI_LE_SET_ADV_PARAMS   0x2006U
#define ACI_OP_HCI_LE_READ_ADV_TX_PWR  0x2007U
#define ACI_OP_HCI_LE_SET_ADV_DATA     0x2008U
#define ACI_OP_HCI_LE_SET_SCAN_RSP     0x2009U
#define ACI_OP_HCI_LE_SET_ADV_ENABLE   0x200aU
#define ACI_OP_HCI_LE_WRITE_SUGG_DLEN  0x2024U
#define ACI_OP_HAL_FW_DETAILS          0xfc01U
#define ACI_OP_HAL_SET_TX_POWER        0xfc0fU
#define ACI_OP_HAL_SET_EVENT_MASK      0xfc1aU
#define ACI_OP_GAP_SET_IO_CAP          0xfc85U
#define ACI_OP_GAP_SET_AUTH_REQ        0xfc86U
#define ACI_OP_GAP_PASS_KEY_RESP       0xfc88U
#define ACI_OP_GAP_INIT                0xfc8aU
#define ACI_OP_GAP_SLAVE_SECURITY_REQ  0xfc8dU
#define ACI_OP_GAP_GET_SECURITY_LEVEL  0xfc90U
#define ACI_OP_GAP_SET_EVENT_MASK      0xfc91U
#define ACI_OP_GAP_TERMINATE           0xfc93U
#define ACI_OP_GAP_CLEAR_SECURITY_DB   0xfc94U
#define ACI_OP_GAP_ALLOW_REBOND        0xfc95U
#define ACI_OP_GAP_RESOLVE_PRIV_ADDR   0xfca0U
#define ACI_OP_GAP_GET_BONDED_DEVICES  0xfca3U
#define ACI_OP_GAP_NUMCMP_CONFIRM      0xfca5U
#define ACI_OP_GAP_REMOVE_BONDED       0xfcaaU
#define ACI_OP_GATT_INIT               0xfd01U
#define ACI_OP_GATT_ADD_SERVICE        0xfd02U
#define ACI_OP_GATT_ADD_CHAR           0xfd04U
#define ACI_OP_GATT_ADD_CHAR_DESC      0xfd05U
#define ACI_OP_GATT_UPDATE_CHAR_VALUE  0xfd06U
#define ACI_OP_GATT_SET_EVENT_MASK     0xfd0aU
#define ACI_OP_GATT_EXCHANGE_CONFIG    0xfd0bU
#define ACI_OP_GATT_DISC_SVC_BY_UUID   0xfd13U
#define ACI_OP_GATT_DISC_ALL_CHARS     0xfd15U
#define ACI_OP_GATT_DISC_CHARS_BY_UUID 0xfd16U
#define ACI_OP_GATT_DISC_ALL_DESCS     0xfd17U
#define ACI_OP_GATT_READ_CHAR_VALUE    0xfd18U
#define ACI_OP_GATT_READ_BY_UUID       0xfd19U
#define ACI_OP_GATT_READ_LONG          0xfd1aU
#define ACI_OP_GATT_WRITE_CHAR_VALUE   0xfd1cU
#define ACI_OP_GATT_WRITE_LONG         0xfd1dU
#define ACI_OP_GATT_WRITE_NO_RSP       0xfd23U
#define ACI_OP_GATT_CONFIRM_INDICATION 0xfd25U
#define ACI_OP_GATT_WRITE_RESP         0xfd26U
#define ACI_OP_GATT_ALLOW_READ         0xfd27U
#define ACI_OP_GATT_SET_DESC_VALUE     0xfd29U
#define ACI_OP_GATT_UPDATE_CHAR_EXT    0xfd2cU
#define ACI_OP_GATT_DENY_READ          0xfd2dU
#define ACI_OP_GATT_SET_ACCESS_PERM    0xfd2eU
#define ACI_OP_L2CAP_CONN_PARAM_UPDATE 0xfd81U

// --- Events ------------------------------------------------------------------------------------

#define ACI_EVT_DISCONN_COMPLETE  0x05U
#define ACI_EVT_ENC_CHANGE        0x08U
#define ACI_EVT_CMD_COMPLETE      0x0eU
#define ACI_EVT_CMD_STATUS        0x0fU
#define ACI_EVT_HW_ERROR          0x10U
#define ACI_EVT_NUM_COMP_PKTS     0x13U
#define ACI_EVT_ENC_KEY_REFRESH   0x30U
#define ACI_EVT_LE_META           0x3eU
#define ACI_EVT_VS                0xffU
// Ours, from the link to the host: the chip is up (1) or went down by itself (0)
#define ACI_EVT_LINK              0x00U

#define ACI_LE_CONN_COMPLETE      0x01U
#define ACI_LE_CONN_UPDATE        0x03U
#define ACI_LE_DATA_LEN_CHANGE    0x07U
#define ACI_LE_ENH_CONN_COMPLETE  0x0aU

#define ACI_VS_BLUE_INITIALIZED   0x0001U
#define ACI_VS_BLUE_EVENTS_LOST   0x0002U
#define ACI_VS_BLUE_CRASH_INFO    0x0003U
#define ACI_VS_GAP_PAIRING_CMPLT  0x0401U
#define ACI_VS_GAP_PASS_KEY_REQ   0x0402U
#define ACI_VS_GAP_SLAVE_SEC_INIT 0x0404U
#define ACI_VS_GAP_BOND_LOST      0x0405U
#define ACI_VS_GAP_PROC_COMPLETE  0x0407U
#define ACI_VS_GAP_NUMCMP_VALUE   0x0409U
#define ACI_VS_L2CAP_UPDATE_RESP  0x0800U
#define ACI_VS_L2CAP_PROC_TIMEOUT 0x0801U
#define ACI_VS_GATT_ATTR_MODIFIED 0x0c01U
#define ACI_VS_GATT_PROC_TIMEOUT  0x0c02U
#define ACI_VS_ATT_MTU_RESP       0x0c03U
#define ACI_VS_ATT_FIND_INFO_RESP 0x0c04U
#define ACI_VS_ATT_FIND_BY_TYPE   0x0c05U
#define ACI_VS_ATT_READ_BY_TYPE   0x0c06U
#define ACI_VS_ATT_READ_RESP      0x0c07U
#define ACI_VS_ATT_READ_BLOB_RESP 0x0c08U
#define ACI_VS_GATT_INDICATION    0x0c0eU
#define ACI_VS_GATT_NOTIFICATION  0x0c0fU
#define ACI_VS_GATT_PROC_COMPLETE 0x0c10U
#define ACI_VS_GATT_ERROR_RESP    0x0c11U
#define ACI_VS_GATT_READ_BY_UUID  0x0c12U
#define ACI_VS_GATT_WRITE_PERMIT  0x0c13U
#define ACI_VS_GATT_READ_PERMIT   0x0c14U
#define ACI_VS_GATT_TX_POOL_AVAIL 0x0c16U
#define ACI_VS_GATT_SERVER_CONF   0x0c17U

// --- Status codes ------------------------------------------------------------------------------

#define ACI_STATUS_INSUFFICIENT_RESOURCES 0x64U
#define ACI_STATUS_BUSY                   0x43U
#define ACI_STATUS_NOT_ALLOWED            0x46U

// The most parameters a command or an event carries
#define ACI_MAX_PARAMS 255U

static inline uint16_t aci_get_le16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t aci_get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint8_t *aci_put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  return p + 2;
}

static inline uint8_t *aci_put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
  return p + 4;
}

// --- The link to the chip (link.c) -------------------------------------------------------------

/**
 * Sends a command and waits for its Command Complete or Command Status, from any thread but the
 * link's. Its status (an HCI or ACI status code), or a negative errno: -ENOTCONN when the chip is
 * not up, -ETIMEDOUT. The return parameters after the status go to rsp, up to *rsp_len, which is
 * set to their length; rsp may be NULL.
 */
int aci_cmd(uint16_t op, const void *par, uint8_t par_len, void *rsp, size_t *rsp_len);

/** Asks for the chip to be started; ACI_EVT_LINK follows on the host's queue. */
void aci_link_start(void);

/** Puts the chip in reset and waits for it. */
void aci_link_stop(void);

/** The identity address read at the chip's last start, or -ENOENT. */
int aci_link_identity(uint8_t addr[6]);

/** The host's handler of the chip's events, on the host's thread: code, length, parameters. */
void aci_hs_rx(const uint8_t *evt, size_t len);

/** Starts the link's thread. */
void aci_link_init(void);

// --- The host (hs.c) ---------------------------------------------------------------------------

/** ble_hs_* errors from a chip status, or a negative errno of aci_cmd(). */
int aci_hs_err(int status);

/** The ble_hs_lock() the host's state is under. */
void aci_lock(void);
void aci_unlock(void);

// --- GAP (gap.c) -------------------------------------------------------------------------------

void aci_gap_init(void);
/** The chip is set up: the GAP's part of it. */
int aci_gap_setup(void);
/** The chip went down: every connection is gone, advertising too. */
void aci_gap_reset(int reason);
void aci_gap_rx_hci(uint8_t code, const uint8_t *p, size_t len);
void aci_gap_rx_le(uint8_t sub, const uint8_t *p, size_t len);
/** A vendor event of the GAP or L2CAP; false if not one. */
bool aci_gap_rx_vs(uint16_t code, const uint8_t *p, size_t len);
/** Calls the listeners, then the connection's callback; the latter's return. */
int aci_gap_event(struct ble_gap_event *event, uint16_t conn_handle);
/** Whether a connection is up. */
bool aci_gap_conn_exists(uint16_t conn_handle);
void aci_gap_set_mtu(uint16_t conn_handle, uint16_t mtu);
/** The chip's GAP service and its Device Name characteristic, from aci_gap_init. */
void aci_gap_name_handles(uint16_t *svc, uint16_t *name_chr);

// --- GATT server (gatts.c) ---------------------------------------------------------------------

void aci_gatts_init(void);
/** Adds the registered services to the chip. */
int aci_gatts_setup(void);
void aci_gatts_conn_closed(uint16_t conn_handle);
bool aci_gatts_rx_vs(uint16_t code, const uint8_t *p, size_t len);

// --- GATT client (gattc.c) ---------------------------------------------------------------------

void aci_gattc_init(void);
void aci_gattc_conn_closed(uint16_t conn_handle);
bool aci_gattc_rx_vs(uint16_t code, const uint8_t *p, size_t len);

// --- Store (store_util.c) ----------------------------------------------------------------------

/** The bonds of the chip and of the store made to agree. */
void aci_store_reconcile(void);
/** A bond made: its record in the store. */
void aci_store_bonded(const ble_addr_t *peer, bool authenticated);
/** The chip's bonds, up to max: their number, or -1. */
int aci_chip_bonds(ble_addr_t *out, int max);
/** Whether a bond the chip listed is of a peer. */
bool aci_chip_bond_match(const ble_addr_t *listed, const ble_addr_t *addr);

// --- Services (svc.c) --------------------------------------------------------------------------

/** The device name to the chip's GAP service. */
int aci_svc_setup(void);
