/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @defgroup fat_ro Read-only FAT
 * @ingroup util
 * @brief Files of the root directory of a FAT16, FAT32 or exFAT volume, read only.
 *
 * Enough to take a file from a drive a computer wrote, as an update drive: the volume is found at
 * the start of the device or in the first partition of its MBR, in 512-byte sectors. Only the
 * root directory is listed, and files by where they are, not by name: long names are not read.
 *
 * @code{.c}
 * static int prv_found(struct fat_ro *fs, struct fat_ro_file *file, void *arg) {
 *   return file->size == 1234U ? 1 : 0;  // stop at the file
 * }
 *
 * ret = fat_ro_mount(&s_fs, prv_read_blocks, nullptr, num_blocks);
 * ret = fat_ro_foreach_root_file(&s_fs, prv_found, nullptr);
 * @endcode
 * @{
 */

/** @brief Size of a sector, the only one supported. */
#define FAT_RO_SECTOR_SIZE 512U

/** @brief Read @p count sectors from @p sector of the device. Returns 0 or a negative errno. */
typedef int (*fat_ro_read_fn)(void *ctx, uint32_t sector, void *buf, uint32_t count);

/** @brief Kind of volume. */
enum fat_ro_type {
  FAT_RO_FAT16 = 1,
  FAT_RO_FAT32,
  FAT_RO_EXFAT,
};

/** @brief A mounted volume, its buffers included. */
struct fat_ro {
  /** @cond INTERNAL_HIDDEN */
  fat_ro_read_fn read;
  void *ctx;
  uint32_t dev_sectors;
  /** @endcond */
  /** Kind of volume. */
  enum fat_ro_type type;
  /** @cond INTERNAL_HIDDEN */
  uint32_t fat_sector;
  uint32_t data_sector;
  uint32_t cluster_shift;
  uint32_t num_clusters;
  uint32_t root_cluster;
  uint32_t root_sector;
  uint32_t root_sectors;
  uint32_t fat_cached;
  uint8_t dir_buf[FAT_RO_SECTOR_SIZE] __attribute__((aligned(4)));
  uint8_t fat_buf[FAT_RO_SECTOR_SIZE] __attribute__((aligned(4)));
  uint8_t data_buf[FAT_RO_SECTOR_SIZE] __attribute__((aligned(4)));
  /** @endcond */
};

/** @brief A file of the root directory, with the position of the last read. */
struct fat_ro_file {
  /** Size, in bytes. */
  uint32_t size;
  /** First cluster, 0 for an empty file. */
  uint32_t first_cluster;
  /** Clusters one after the other, without a FAT chain (exFAT). */
  bool contiguous;
  /** @cond INTERNAL_HIDDEN */
  uint32_t pos_index;
  uint32_t pos_cluster;
  /** @endcond */
};

/**
 * @brief Called for each file of the root directory.
 *
 * @param fs Volume.
 * @param file File, which fat_ro_read() may read.
 * @param arg Argument of fat_ro_foreach_root_file().
 * @return 0 to go on, a positive value to stop, returned, or a negative errno.
 */
typedef int (*fat_ro_file_cb)(struct fat_ro *fs, struct fat_ro_file *file, void *arg);

/**
 * @brief Find the volume on a device.
 *
 * @param fs Volume.
 * @param read Reads sectors of the device.
 * @param ctx Argument of @p read.
 * @param dev_sectors Sectors of the device, which the volume must be within.
 * @return 0, @c -ENOENT without a volume it reads, or another negative errno.
 */
int fat_ro_mount(struct fat_ro *fs, fat_ro_read_fn read, void *ctx, uint32_t dev_sectors);

/**
 * @brief Go through the files of the root directory, leaving out directories and labels.
 *
 * @param fs Volume.
 * @param cb Called for each file.
 * @param arg Argument of @p cb.
 * @return 0 after the last file, what @p cb stopped with, or a negative errno.
 */
int fat_ro_foreach_root_file(struct fat_ro *fs, fat_ro_file_cb cb, void *arg);

/**
 * @brief Read from a file.
 *
 * Reads go fastest in order, and in whole sectors.
 *
 * @param fs Volume.
 * @param file File.
 * @param offset Offset in the file.
 * @param buf Buffer.
 * @param len Number of bytes, all within the file.
 * @return 0, @c -EINVAL past the end of the file, or another negative errno.
 */
int fat_ro_read(struct fat_ro *fs, struct fat_ro_file *file, uint32_t offset, void *buf,
                size_t len);

/** @} */
