/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @defgroup vfat Virtual FAT
 * @ingroup util
 * @brief A read-only FAT16 volume computed as it is read, from a list of files.
 *
 * Nothing of the volume is stored: the boot sector, the two FATs and the root directory are
 * computed for each sector read, and the data of a file comes from a callback. Each file takes
 * clusters one after the other, in the order of the list. Names that are not 8.3 names get long
 * file name entries (VFAT), with an 8.3 alias of their first characters and a number. The volume
 * has no partition table (a "superfloppy"), in 512-byte sectors.
 *
 * @code{.c}
 * static int prv_data(void *ctx, size_t file, uint32_t offset, void *buf, uint32_t len) {
 *   ... len bytes of file from offset ...
 * }
 * struct vfat_file files[2] = {{.name = "2026-11-28-1045-run.fit", .size = 95123}, ...};
 * vfat_layout(&vol, &(struct vfat_params){.label = "UNA WATCH", .read = prv_data}, files, 2);
 * vfat_read(&vol, lba, buf, count);
 * @endcode
 * @{
 */

/** @brief Size of a sector. */
#define VFAT_SECTOR_SIZE 512U

/** @brief Longest file name, in characters. */
#define VFAT_NAME_MAX 63U

/** @brief Root directory entries, the FAT16 default. */
#define VFAT_ROOT_ENTRIES 512U

/**
 * @brief Read @p len bytes of file @p file, by its index in the list, from @p offset, all within
 * the file. Returns 0 or a negative errno.
 */
typedef int (*vfat_read_fn)(void *ctx, size_t file, uint32_t offset, void *buf, uint32_t len);

/** @brief A file of the volume. */
struct vfat_file {
  /** Name, printable ASCII, without / \ : * ? " < > |. */
  char name[VFAT_NAME_MAX + 1U];
  /** Size, in bytes. */
  uint32_t size;
  /** Time of the file in FAT's encoding, local time, for its creation and its last write. */
  uint16_t date;
  uint16_t time;
  /** @cond INTERNAL_HIDDEN */
  char sfn[11];
  uint32_t first_cluster;
  uint32_t num_clusters;
  uint16_t first_entry;
  uint8_t num_lfn;
  /** @endcond */
};

/** @brief What the volume is made of. */
struct vfat_params {
  /** Sectors of the volume. 0 for 128 MiB. */
  uint32_t total_sectors;
  /** Sectors a cluster; a power of 2. 0 for 8, 4 KiB. */
  uint8_t sectors_per_cluster;
  /** Volume label, up to 11 characters, upper case. */
  const char *label;
  /** Volume serial number. */
  uint32_t serial;
  /** Date and time of the label, FAT's encoding. */
  uint16_t date;
  uint16_t time;
  /** The data of the files. */
  vfat_read_fn read;
  void *ctx;
};

/** @brief A volume, laid out. */
struct vfat {
  /** @cond INTERNAL_HIDDEN */
  struct vfat_params p;
  struct vfat_file *files;
  size_t num_files;
  uint32_t reserved;
  uint32_t fat_sectors;
  uint32_t root_sector;
  uint32_t data_sector;
  uint32_t num_clusters;
  uint16_t num_entries;
  /** @endcond */
};

/**
 * @brief Lay a volume out: clusters and directory entries of the files, in the order given. Files
 * that do not fit, in the clusters or in the root directory, are left out from the end.
 *
 * @param v Volume.
 * @param p Parameters, copied.
 * @param files Files, kept by the volume: they must outlive it, and their names and sizes not
 * change.
 * @param num Number of files.
 * @return How many of the files the volume has, or a negative errno: @c -EINVAL for parameters
 * that make no FAT16 volume, or a name that is no file name.
 */
int vfat_layout(struct vfat *v, const struct vfat_params *p, struct vfat_file *files, size_t num);

/** @brief Sectors of the volume. */
uint32_t vfat_num_sectors(const struct vfat *v);

/**
 * @brief Read sectors of the volume.
 *
 * @param v Volume.
 * @param lba First sector.
 * @param buf @p count sectors.
 * @param count Number of sectors.
 * @return 0, @c -EINVAL past the end, or what the read callback returned.
 */
int vfat_read(const struct vfat *v, uint32_t lba, void *buf, uint32_t count);

/**
 * @brief The file a sector holds data of, if any.
 *
 * @param v Volume.
 * @param lba Sector.
 * @return Index of the file, or -1.
 */
int vfat_file_at(const struct vfat *v, uint32_t lba);

/** @brief FAT's encoding of a date, from 1980 to 2107; earlier ones are 1980-01-01. */
uint16_t vfat_date(unsigned int year, unsigned int month, unsigned int day);

/** @brief FAT's encoding of a time, to two seconds. */
uint16_t vfat_time(unsigned int hour, unsigned int minute, unsigned int second);

/** @} */
