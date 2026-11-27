/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "resource_storage_volume.h"

#include "resource_storage_impl.h"

#include <inttypes.h>

#include <pbl/boot_trace/boot_trace.h>
#include <pbl/drivers/flash.h>
#include <pbl/drivers/mmc.h>
#include <pbl/fat_ro/fat_ro.h>
#include <pbl/kernel/malloc.h>
#include <pbl/logging/logging.h>
#include <pbl/util/math.h>

#define CHUNK_SIZE SUBSECTOR_SIZE_BYTES

struct install {
  struct fat_ro fs;
  struct fat_ro_file file;
  const ResourceVersion *version;
  uint32_t max_size;
  uint8_t chunk[CHUNK_SIZE] __attribute__((aligned(4)));
};

static int prv_read_sectors(void *ctx, uint32_t sector, void *buf, uint32_t count) {
  return pbl_mmc_read(MMC, CONFIG_RESOURCES_VOLUME_FIRST_BLOCK + sector, buf, count);
}

static int prv_match(struct fat_ro *fs, struct fat_ro_file *file, void *arg) {
  struct install *inst = arg;
  ResourceManifest manifest;

  if (file->size < MANIFEST_SIZE || file->size > inst->max_size ||
      fat_ro_read(fs, file, 0U, &manifest, sizeof(manifest)) != 0 ||
      !resource_version_matches(&manifest.version, inst->version)) {
    return 0;
  }

  inst->file = *file;

  return 1;
}

static bool prv_copy(struct install *inst, const SystemResourceBank *bank) {
  const uint32_t size = inst->file.size;
  int ret;

  PBL_BOOT_TRACE(PBL_BT_RES, PBL_BT_RES_ERASE);
  ret = pbl_flash_erase(FLASH, bank->begin,
                        PBL_DIV_ROUND_UP(size, SUBSECTOR_SIZE_BYTES) * SUBSECTOR_SIZE_BYTES);
  if (ret != 0) {
    PBL_LOG_ERR("Erasing the bank failed: %d", ret);
    return false;
  }

  for (uint32_t offset = 0U; offset < size; offset += CHUNK_SIZE) {
    const uint32_t n = PBL_MIN(size - offset, CHUNK_SIZE);

    PBL_BOOT_TRACE(PBL_BT_RES, PBL_BT_RES_COPY | ((offset / CHUNK_SIZE) & 0xFFFFU));
    PBL_BOOT_TRACE_NOTE(PBL_BT_NOTE_RES_COPIED, offset);
    ret = fat_ro_read(&inst->fs, &inst->file, offset, inst->chunk, n);
    if (ret != 0) {
      PBL_LOG_ERR("Reading the resources failed at %" PRIu32 ": %d", offset, ret);
      return false;
    }

    ret = pbl_flash_write(FLASH, bank->begin + offset, inst->chunk, n);
    if (ret != 0) {
      PBL_LOG_ERR("Writing the resources failed at %" PRIu32 ": %d", offset, ret);
      return false;
    }
  }

  return true;
}

bool resource_storage_volume_install(const SystemResourceBank *bank,
                                     const ResourceVersion *version) {
  struct install *inst;
  bool installed = false;
  int ret;

  inst = pbl_malloc(sizeof(*inst));
  if (inst == nullptr) {
    return false;
  }
  inst->version = version;
  inst->max_size = bank->end - bank->begin;

  PBL_BOOT_TRACE(PBL_BT_RES, PBL_BT_RES_MOUNT);
  ret = fat_ro_mount(&inst->fs, prv_read_sectors, nullptr, CONFIG_RESOURCES_VOLUME_BLOCKS);
  if (ret != 0) {
    PBL_LOG_ERR("No update drive: %d", ret);
    goto out;
  }

  PBL_BOOT_TRACE(PBL_BT_RES, PBL_BT_RES_SEARCH);
  ret = fat_ro_foreach_root_file(&inst->fs, prv_match, inst);
  if (ret <= 0) {
    PBL_LOG_ERR("No resources <%#010" PRIx32 ", %" PRIu32 "> on the update drive: %d", version->crc,
                version->timestamp, ret);
    goto out;
  }

  PBL_LOG_INFO("Copying %" PRIu32 " bytes of resources from the update drive", inst->file.size);
  installed = prv_copy(inst, bank);
  if (!installed) {
    PBL_BOOT_TRACE(PBL_BT_RES, PBL_BT_RES_INSTALL_FAILED);
  }

out:
  pbl_free(inst);

  return installed;
}
