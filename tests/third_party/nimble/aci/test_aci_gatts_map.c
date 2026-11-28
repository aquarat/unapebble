/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <string.h>

#include <gatts_map.h>
#include <host/ble_att.h>
#include <host/ble_gatt.h>
#include <host/ble_uuid.h>

static int prv_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
  return 0;
}

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xda, 0xda, 0x9b, 0x69, 0xa6, 0x1a, 0x42, 0xc6, 0xbb, 0x0f, 0x8e, 0x32, 0x00, 0x00, 0x00, 0x40);
static const ble_uuid128_t s_data_uuid = BLE_UUID128_INIT(
    0xda, 0xda, 0x9b, 0x69, 0xa6, 0x1a, 0x42, 0xc6, 0xbb, 0x0f, 0x8e, 0x32, 0x01, 0x00, 0x00, 0x40);

static struct ble_gatt_dsc_def s_dscs[] = {
  {
    .uuid = BLE_UUID16_DECLARE(0x2901),
    .att_flags = BLE_ATT_F_READ,
    .access_cb = prv_access,
  },
  {0},
};

static uint16_t s_notify_handle;
static uint16_t s_write_handle;

// A service as PebbleOS defines them: notify, write without response, read and write, with a
// descriptor
static const struct ble_gatt_svc_def s_svcs[] = {
  {
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &s_svc_uuid.u,
    .characteristics =
        (struct ble_gatt_chr_def[]){
          {
            .uuid = &s_data_uuid.u,
            .access_cb = prv_access,
            .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
            .val_handle = &s_notify_handle,
          },
          {
            .uuid = BLE_UUID16_DECLARE(0x2a19),
            .access_cb = prv_access,
            .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            .val_handle = &s_write_handle,
          },
          {
            .uuid = BLE_UUID16_DECLARE(0x2a00),
            .access_cb = prv_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            .descriptors = s_dscs,
          },
          {0},
        },
  },
  {0},
};

static struct aci_chr s_chrs[4];
static struct aci_dsc s_dsc_tab[2];
static struct aci_attr_table s_table;

void test_aci_gatts_map__initialize(void) {
  memset(s_chrs, 0, sizeof(s_chrs));
  memset(s_dsc_tab, 0, sizeof(s_dsc_tab));
  s_table = (struct aci_attr_table){
    .chrs = s_chrs,
    .max_chrs = 3,
    .dscs = s_dsc_tab,
    .max_dscs = 1,
  };
}

void test_aci_gatts_map__uuid16(void) {
  uint8_t out[16];

  cl_assert_equal_i(aci_map_uuid(BLE_UUID16_DECLARE(0xfed9), out), ACI_UUID_16);
  cl_assert_equal_i(out[0], 0xd9);
  cl_assert_equal_i(out[1], 0xfe);
}

void test_aci_gatts_map__uuid128_as_is(void) {
  uint8_t out[16];

  cl_assert_equal_i(aci_map_uuid(&s_svc_uuid.u, out), ACI_UUID_128);
  cl_assert(memcmp(out, s_svc_uuid.value, 16) == 0);
}

// The chip takes 16 or 128 bits: a 32-bit UUID goes on the Bluetooth Base UUID
void test_aci_gatts_map__uuid32_as_128(void) {
  static const uint8_t expected[16] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                       0x00, 0x10, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12};
  uint8_t out[16];

  cl_assert_equal_i(aci_map_uuid(BLE_UUID32_DECLARE(0x12345678), out), ACI_UUID_128);
  cl_assert(memcmp(out, expected, 16) == 0);
}

void test_aci_gatts_map__properties(void) {
  cl_assert_equal_i(aci_map_chr_props(BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY), 0x12);
  cl_assert_equal_i(aci_map_chr_props(BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC),
                    0x04);
  // A reliable write is announced in the extended properties
  cl_assert_equal_i(aci_map_chr_props(BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_RELIABLE_WRITE),
                    0x88);
}

void test_aci_gatts_map__permissions(void) {
  cl_assert_equal_i(aci_map_chr_perms(BLE_GATT_CHR_F_READ), 0);
  cl_assert_equal_i(aci_map_chr_perms(BLE_GATT_CHR_F_READ_ENC), ACI_PERM_ENCRY_READ);
  // Authentication goes with encryption
  cl_assert_equal_i(aci_map_chr_perms(BLE_GATT_CHR_F_WRITE_AUTHEN),
                    ACI_PERM_AUTHEN_WRITE | ACI_PERM_ENCRY_WRITE);
  cl_assert_equal_i(aci_map_chr_perms(BLE_GATT_CHR_F_READ_AUTHOR | BLE_GATT_CHR_F_WRITE_ENC),
                    ACI_PERM_AUTHOR_READ | ACI_PERM_ENCRY_WRITE);
}

void test_aci_gatts_map__event_masks(void) {
  // Read: answered by the host
  cl_assert_equal_i(aci_map_chr_evt_mask(BLE_GATT_CHR_F_READ), ACI_GATT_NOTIFY_READ_REQ_WAIT);
  // Write without response alone: reported only
  cl_assert_equal_i(aci_map_chr_evt_mask(BLE_GATT_CHR_F_WRITE_NO_RSP),
                    ACI_GATT_NOTIFY_ATTRIBUTE_WRITE);
  // A write that may be refused waits, even with write without response besides
  cl_assert_equal_i(aci_map_chr_evt_mask(BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP),
                    ACI_GATT_NOTIFY_WRITE_REQ_WAIT);
  // Notify: its CCCD's writes are reported
  cl_assert_equal_i(aci_map_chr_evt_mask(BLE_GATT_CHR_F_NOTIFY), ACI_GATT_NOTIFY_ATTRIBUTE_WRITE);
  cl_assert_equal_i(aci_map_chr_evt_mask(BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE),
                    ACI_GATT_NOTIFY_READ_REQ_WAIT | ACI_GATT_NOTIFY_WRITE_REQ_WAIT);
}

void test_aci_gatts_map__value_lengths(void) {
  // Data: as much as the MTU takes
  cl_assert_equal_i(aci_map_chr_max_len(BLE_GATT_CHR_F_NOTIFY, -1), ACI_CHR_VALUE_MAX);
  cl_assert_equal_i(aci_map_chr_max_len(BLE_GATT_CHR_F_WRITE_NO_RSP, -1), ACI_CHR_VALUE_MAX);
  // State: what one read at the default MTU gives, or its value now if longer
  cl_assert_equal_i(aci_map_chr_max_len(BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, -1),
                    ACI_CHR_STATE_MIN);
  cl_assert_equal_i(aci_map_chr_max_len(BLE_GATT_CHR_F_READ, 4), ACI_CHR_STATE_MIN);
  cl_assert_equal_i(aci_map_chr_max_len(BLE_GATT_CHR_F_READ, 29), 29);
  cl_assert_equal_i(aci_map_chr_max_len(BLE_GATT_CHR_F_READ, 600), ACI_CHR_VALUE_MAX);
}

void test_aci_gatts_map__descriptor_flags(void) {
  cl_assert_equal_i(aci_map_dsc_access(BLE_ATT_F_READ | BLE_ATT_F_WRITE),
                    ACI_ACCESS_READ | ACI_ACCESS_WRITE);
  cl_assert_equal_i(aci_map_dsc_perms(BLE_ATT_F_READ_ENC | BLE_ATT_F_WRITE_AUTHEN),
                    ACI_PERM_ENCRY_READ | ACI_PERM_AUTHEN_WRITE | ACI_PERM_ENCRY_WRITE);
  cl_assert_equal_i(aci_map_dsc_evt_mask(BLE_ATT_F_READ), ACI_GATT_NOTIFY_READ_REQ_WAIT);
}

// The service, every declaration, value, CCCD and descriptor: what aci_gatt_add_service reserves
void test_aci_gatts_map__attribute_count(void) {
  size_t num_chrs;
  size_t num_dscs;

  // 1 + (2 + CCCD) + 2 + (2 + descriptor)
  cl_assert_equal_i(aci_svc_num_attrs(&s_svcs[0]), 9);

  aci_svcs_count(s_svcs, &num_chrs, &num_dscs);
  cl_assert_equal_i(num_chrs, 3);
  cl_assert_equal_i(num_dscs, 1);
}

// The chip gives the declaration's handle: the value is next, then the CCCD of one that notifies
void test_aci_gatts_map__handles_from_the_declaration(void) {
  const struct ble_gatt_chr_def *chrs = s_svcs[0].characteristics;

  cl_assert_equal_i(aci_table_add_chr(&s_table, &s_svcs[0], &chrs[0], 0x0025, 0x0026, 217), 0);
  cl_assert_equal_i(aci_table_add_chr(&s_table, &s_svcs[0], &chrs[1], 0x0025, 0x0029, 217), 1);

  cl_assert_equal_i(s_chrs[0].val_handle, 0x0027);
  cl_assert_equal_i(s_chrs[0].cccd_handle, 0x0028);
  cl_assert_equal_i(s_chrs[0].end_handle, 0x0028);
  cl_assert_equal_i(s_chrs[0].evt_mask, ACI_GATT_NOTIFY_ATTRIBUTE_WRITE);
  cl_assert_equal_i(s_chrs[1].val_handle, 0x002a);
  cl_assert_equal_i(s_chrs[1].cccd_handle, 0);
  cl_assert_equal_i(s_chrs[1].end_handle, 0x002a);
  cl_assert_equal_i(s_chrs[1].max_len, 217);
}

void test_aci_gatts_map__find(void) {
  const struct ble_gatt_chr_def *chrs = s_svcs[0].characteristics;
  size_t chr_idx = 99;
  size_t dsc_idx = 99;

  (void)aci_table_add_chr(&s_table, &s_svcs[0], &chrs[0], 0x0025, 0x0026, 217);
  (void)aci_table_add_chr(&s_table, &s_svcs[0], &chrs[2], 0x0025, 0x0029, 20);
  cl_assert_equal_i(aci_table_add_dsc(&s_table, 1, &s_dscs[0], 0x002b), 0);
  cl_assert_equal_i(s_chrs[1].end_handle, 0x002b);

  cl_assert_equal_i(aci_table_find(&s_table, 0x0027, &chr_idx, &dsc_idx), ACI_ATTR_VALUE);
  cl_assert_equal_i(chr_idx, 0);
  cl_assert_equal_i(aci_table_find(&s_table, 0x0028, &chr_idx, &dsc_idx), ACI_ATTR_CCCD);
  cl_assert_equal_i(chr_idx, 0);
  cl_assert_equal_i(aci_table_find(&s_table, 0x002a, &chr_idx, &dsc_idx), ACI_ATTR_VALUE);
  cl_assert_equal_i(chr_idx, 1);
  cl_assert_equal_i(aci_table_find(&s_table, 0x002b, &chr_idx, &dsc_idx), ACI_ATTR_DSC);
  cl_assert_equal_i(chr_idx, 1);
  cl_assert_equal_i(dsc_idx, 0);
  cl_assert(s_dsc_tab[dsc_idx].dsc == &s_dscs[0]);

  // Declarations, the service and handles of others are no attribute of the host
  cl_assert_equal_i(aci_table_find(&s_table, 0x0026, &chr_idx, &dsc_idx), ACI_ATTR_NONE);
  cl_assert_equal_i(aci_table_find(&s_table, 0x0025, &chr_idx, &dsc_idx), ACI_ATTR_NONE);
  cl_assert_equal_i(aci_table_find(&s_table, 0x0000, &chr_idx, &dsc_idx), ACI_ATTR_NONE);
  cl_assert_equal_i(aci_table_find(&s_table, 0x0003, &chr_idx, &dsc_idx), ACI_ATTR_NONE);

  cl_assert_equal_i(aci_table_find_val(&s_table, 0x002a), 1);
  cl_assert_equal_i(aci_table_find_val(&s_table, 0x0028), -1);
}

void test_aci_gatts_map__full(void) {
  const struct ble_gatt_chr_def *chrs = s_svcs[0].characteristics;

  for (int i = 0; i < 3; i++) {
    cl_assert_equal_i(aci_table_add_chr(&s_table, &s_svcs[0], &chrs[0], 1, 2 + 3 * i, 20), i);
  }
  cl_assert_equal_i(aci_table_add_chr(&s_table, &s_svcs[0], &chrs[0], 1, 20, 20), -1);

  cl_assert_equal_i(aci_table_add_dsc(&s_table, 0, &s_dscs[0], 30), 0);
  cl_assert_equal_i(aci_table_add_dsc(&s_table, 0, &s_dscs[0], 31), -1);
  // Of a characteristic that is not there
  s_table.max_dscs = 2;
  cl_assert_equal_i(aci_table_add_dsc(&s_table, 7, &s_dscs[0], 31), -1);
}
