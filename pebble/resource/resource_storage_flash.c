/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "resource_storage_flash.h"

#include "resource_storage_impl.h"
#ifdef CONFIG_RESOURCES_FROM_VOLUME
#include "resource_storage_volume.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <pbl/board/partitions.h>
#include <pbl/boot_trace/boot_trace.h>
#include <pbl/crc/crc.h>
#include <pbl/drivers/flash.h>
#include <pbl/logging/logging.h>
#include <pbl/services/process_management/app_storage.h>
#include <pbl/util/size.h>

#include <resource/resource_version.auto.h>
#include <system/bootbits.h>

#ifdef CONFIG_RESOURCES_IN_CODE_FLASH
// One bank, installed with the image in the flash the code runs from, offsets from FLASH_BASE:
// read in place, never written
static const SystemResourceBank s_resource_banks[] = {
  {
    .begin = CONFIG_RESOURCES_CODE_FLASH_OFFSET,
    .end = CONFIG_RESOURCES_CODE_FLASH_OFFSET + CONFIG_RESOURCES_CODE_FLASH_SIZE,
  },
};
#else
static const SystemResourceBank s_resource_banks[] = {
  {
    .begin = PART_SYSTEM_RESOURCES_BANK_0_OFFSET,
    .end = PART_SYSTEM_RESOURCES_BANK_0_OFFSET + PART_SYSTEM_RESOURCES_BANK_0_SIZE,
  },
  {
    .begin = PART_SYSTEM_RESOURCES_BANK_1_OFFSET,
    .end = PART_SYSTEM_RESOURCES_BANK_1_OFFSET + PART_SYSTEM_RESOURCES_BANK_1_SIZE,
  },
};
#endif

//! Index into s_resource_banks
static unsigned int s_active_bank = 0;
#define BANK s_resource_banks[s_active_bank]

//! Set to true if we've scanned the available resource banks and determined one of them had valid
//! resources in it.
static bool s_valid_resources_found = false;

const ResourceStoreImplementation g_system_bank_impl;

#ifdef CONFIG_RESOURCES_FROM_VOLUME
static unsigned int prv_bank_to_replace(void) {
  unsigned int oldest = 0;
  uint32_t oldest_timestamp = UINT32_MAX;

  for (unsigned int i = 0; i < PBL_ARRAY_SIZE(s_resource_banks); i++) {
    ResourceManifest manifest;

    pbl_flash_read(FLASH, s_resource_banks[i].begin, &manifest, sizeof(manifest));
    // Erased or cleared
    if (manifest.num_resources == 0 || manifest.num_resources > MAX_RESOURCES_FOR_SYSTEM_STORE) {
      return i;
    }
    if (manifest.version.timestamp < oldest_timestamp) {
      oldest = i;
      oldest_timestamp = manifest.version.timestamp;
    }
  }

  return oldest;
}
#endif

static void resource_storage_system_bank_init(void) {
  boot_bit_clear(BOOT_BIT_NEW_SYSTEM_RESOURCES_AVAILABLE);

  ResourceStoreEntry entry = {
    .id = 0, // resource id 0 means the store itself
    .impl = &g_system_bank_impl,
    .length = ENTRY_LENGTH_UNSET
  };

  // Increment s_active_bank and call resource_storage_generic_check for each value to find
  // a bank that's valid.
  for (s_active_bank = 0; s_active_bank < PBL_ARRAY_SIZE(s_resource_banks); ++s_active_bank) {
    PBL_LOG_INFO("Checking bank %u for system resources", s_active_bank);
    PBL_BOOT_TRACE(PBL_BT_RES, PBL_BT_RES_CHECK_BANK | s_active_bank);
    if (resource_storage_generic_check(SYSTEM_APP, 0, &entry, &SYSTEM_RESOURCE_VERSION)) {
      PBL_LOG_INFO("Valid system resources found!");
      PBL_BOOT_TRACE(PBL_BT_RES, PBL_BT_RES_FOUND | s_active_bank);
      s_valid_resources_found = true;
      return;
    }
  }

#ifdef CONFIG_RESOURCES_FROM_VOLUME
  // Into the bank with no resources or the oldest, keeping the other for a firmware going back
  s_active_bank = prv_bank_to_replace();
  PBL_BOOT_TRACE(PBL_BT_RES, PBL_BT_RES_INSTALL | s_active_bank);
  if (resource_storage_volume_install(&BANK, &SYSTEM_RESOURCE_VERSION) &&
      resource_storage_generic_check(SYSTEM_APP, 0, &entry, &SYSTEM_RESOURCE_VERSION)) {
    PBL_LOG_INFO("System resources installed in bank %u", s_active_bank);
    PBL_BOOT_TRACE(PBL_BT_RES, PBL_BT_RES_INSTALLED);
    s_valid_resources_found = true;
    return;
  }
#endif

  // Welp, we found nothing. Leave s_valid_resources_found as false and when resource_storage_check
  // is called as part of system_resource_init we'll complain and handle missing resources.
}

const SystemResourceBank *resource_storage_flash_get_unused_bank(void) {
#ifdef CONFIG_RESOURCES_IN_CODE_FLASH
  // None to write: the resources only come with the image
  static const SystemResourceBank s_no_bank = {};
  return &s_no_bank;
#else
  size_t unused_bank_index;
  if (s_valid_resources_found) {
    unused_bank_index = (s_active_bank + 1) % PBL_ARRAY_SIZE(s_resource_banks);
  } else {
    static int s_unused_bank_index = -1;

    if (s_unused_bank_index == -1) {
      // A crude form of wear levelling to try and keep BB2s in infra happy
      //
      // For real watches, the only time this should happen is during initial onboarding. (If we
      // are in normal FW, one of the resource banks _must_ be valid.) We only call this once
      // because we want to target the same bank when both are unused so features like resumable
      // resource updates work as expected. We reset the bank on boot to make our watches a little
      // more resilient to the scenario where one of the resource banks has gone completely bad
      s_unused_bank_index = rand() % PBL_ARRAY_SIZE(s_resource_banks);
    }
    unused_bank_index = s_unused_bank_index;
  }
  return &s_resource_banks[unused_bank_index];
#endif
}

static uint32_t resource_storage_system_bank_metadata_size(ResourceStoreEntry *entry) {
  return SYSTEM_STORE_METADATA_BYTES;
}

static uint32_t resource_storage_system_bank_get_crc(ResourceStoreEntry *entry, uint32_t num_bytes,
                                                     uint32_t entry_offset) {
  uint32_t start_offset = resource_store_get_metadata_size(entry) + entry_offset;
#ifdef CONFIG_RESOURCES_IN_CODE_FLASH
  return pbl_crc32_legacy((const void *)(uintptr_t)(CONFIG_FLASH_BASE + BANK.begin + start_offset),
                          num_bytes);
#else
  return pbl_flash_crc32_legacy(FLASH, BANK.begin + start_offset, num_bytes);
#endif
}

static uint32_t resource_storage_system_bank_read(ResourceStoreEntry *entry, uint32_t offset,
                                                  void *data, size_t num_bytes) {
#ifdef CONFIG_RESOURCES_IN_CODE_FLASH
  memcpy(data, (const void *)(uintptr_t)(CONFIG_FLASH_BASE + BANK.begin + offset), num_bytes);
#else
  pbl_flash_read(FLASH, BANK.begin + offset, data, num_bytes);
#endif
  return num_bytes;
}

bool resource_storage_flash_bytes_are_readonly(const void *bytes) {
#ifdef CONFIG_MMAP_RESOURCES
  // Pointers inside the banks are in the always-mapped XIP window.
  for (unsigned int i = 0; i < PBL_ARRAY_SIZE(s_resource_banks); i++) {
    if ((bytes >= (const void *)(uintptr_t)(CONFIG_FLASH_BASE + s_resource_banks[i].begin)) &&
        (bytes < (const void *)(uintptr_t)(CONFIG_FLASH_BASE + s_resource_banks[i].end))) {
      return true;
    }
  }
#endif
  return false;
}

static const uint8_t *resource_storage_system_bank_readonly_bytes(ResourceStoreEntry *entry) {
#ifdef CONFIG_MMAP_RESOURCES
  // Bank is XIP-mapped and readable by unprivileged code too.
  return (const uint8_t *)(uintptr_t)(CONFIG_FLASH_BASE + BANK.begin + entry->offset);
#else
  return nullptr;
#endif
}

static void resource_storage_system_bank_clear(ResourceStoreEntry *entry) {
#ifndef CONFIG_RESOURCES_IN_CODE_FLASH
  uint8_t buffer[MANIFEST_SIZE] = {};
  pbl_flash_write(FLASH, BANK.begin, buffer, MANIFEST_SIZE);
#endif
}

bool resource_storage_system_bank_check(ResAppNum app_num, uint32_t resource_id,
                                        ResourceStoreEntry *entry,
                                        const ResourceVersion *expected_version) {
  if (!s_valid_resources_found) {
    // We determined that we had no valid banks during init(), return false.
    return false;
  }

  // Are we checking the store itself?
  if (resource_id == 0) {
    // We've already verified that the bank was good at init(), just return true.
    return true;
  }

  // We're checking a specific resource, delegate this to the generic method.
  return resource_storage_generic_check(app_num, resource_id, entry, expected_version);
}

static bool resource_storage_system_bank_find_resource(ResourceStoreEntry *entry, ResAppNum app_num,
                                                       uint32_t resource_id) {
  return app_num == SYSTEM_APP && s_valid_resources_found;
}

const ResourceStoreImplementation g_system_bank_impl = {
  .type = ResourceStoreTypeSystemBank,

  .init = resource_storage_system_bank_init,
  .clear = resource_storage_system_bank_clear,
  .check = resource_storage_system_bank_check,

  .metadata_size = resource_storage_system_bank_metadata_size,
  .find_resource = resource_storage_system_bank_find_resource,
  .get_resource = resource_storage_generic_get_resource,

  .get_length = resource_storage_generic_get_length,
  .get_crc = resource_storage_system_bank_get_crc,
  .write = resource_storage_generic_write,
  .read = resource_storage_system_bank_read,
  .readonly_bytes = resource_storage_system_bank_readonly_bytes,

  .watch = resource_storage_generic_watch,
  .unwatch = resource_storage_generic_unwatch,
};
