/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <pbl/fat_ro/fat_ro.h>

#define SS FAT_RO_SECTOR_SIZE

#define DIR_ENTRY_SIZE 32U

// FAT12 below this many clusters, FAT16 below the next
#define FAT16_MIN_CLUSTERS 4085U
#define FAT32_MIN_CLUSTERS 65525U

#define FAT_ATTR_LFN   0x0FU
#define FAT_ATTR_LABEL 0x08U
#define FAT_ATTR_DIR   0x10U
#define FAT_ENTRY_FREE 0xE5U

#define EXFAT_ENTRY_FILE   0x85U
#define EXFAT_ENTRY_STREAM 0xC0U
#define EXFAT_SECONDARY    0x40U
#define EXFAT_NO_FAT_CHAIN 0x02U

#define NO_SECTOR UINT32_MAX

// A cluster chain that ends, as next cluster
#define CHAIN_END 1

static uint16_t prv_get16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t prv_get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int prv_read(struct fat_ro *fs, uint32_t sector, void *buf, uint32_t count) {
  if (sector >= fs->dev_sectors || count > fs->dev_sectors - sector) {
    return -EIO;
  }

  return fs->read(fs->ctx, sector, buf, count);
}

static bool prv_has_signature(const uint8_t *b) {
  return b[510] == 0x55U && b[511] == 0xAAU;
}

static bool prv_is_exfat(const uint8_t *b) {
  return memcmp(&b[3], "EXFAT   ", 8) == 0;
}

static bool prv_is_fat(const uint8_t *b) {
  const uint8_t spc = b[13];

  return (b[0] == 0xEBU || b[0] == 0xE9U) && prv_get16(&b[11]) == SS && spc != 0U &&
         (spc & (spc - 1U)) == 0U && prv_get16(&b[14]) != 0U && b[16] != 0U;
}

static uint32_t prv_log2(uint32_t v) {
  uint32_t n = 0U;

  while (v > 1U) {
    v >>= 1;
    n++;
  }

  return n;
}

static int prv_mount_fat(struct fat_ro *fs, uint32_t base, const uint8_t *b) {
  const uint32_t spc = b[13];
  const uint32_t reserved = prv_get16(&b[14]);
  const uint32_t num_fats = b[16];
  const uint32_t root_entries = prv_get16(&b[17]);
  const uint32_t total = prv_get16(&b[19]) != 0U ? prv_get16(&b[19]) : prv_get32(&b[32]);
  const uint32_t fat_size = prv_get16(&b[22]) != 0U ? prv_get16(&b[22]) : prv_get32(&b[36]);
  const uint32_t root_sectors = (root_entries * DIR_ENTRY_SIZE + SS - 1U) / SS;
  const uint64_t data = (uint64_t)reserved + (uint64_t)num_fats * fat_size + root_sectors;
  uint32_t clusters;

  if (fat_size == 0U || data >= total || (uint64_t)base + total > fs->dev_sectors) {
    return -ENOENT;
  }

  clusters = (total - (uint32_t)data) / spc;
  if (clusters < FAT16_MIN_CLUSTERS) {
    return -ENOTSUP;
  }

  fs->type = clusters < FAT32_MIN_CLUSTERS ? FAT_RO_FAT16 : FAT_RO_FAT32;
  if (fs->type == FAT_RO_FAT16 && root_sectors == 0U) {
    return -ENOENT;
  }

  fs->fat_sector = base + reserved;
  fs->data_sector = base + (uint32_t)data;
  fs->cluster_shift = prv_log2(spc);
  fs->num_clusters = clusters;
  fs->root_cluster = fs->type == FAT_RO_FAT32 ? prv_get32(&b[44]) : 0U;
  fs->root_sector = base + reserved + num_fats * fat_size;
  fs->root_sectors = root_sectors;

  return 0;
}

static int prv_mount_exfat(struct fat_ro *fs, uint32_t base, const uint8_t *b) {
  const uint32_t length_lo = prv_get32(&b[72]);
  const uint32_t length_hi = prv_get32(&b[76]);
  const uint32_t heap = prv_get32(&b[88]);
  const uint32_t shift = b[109];

  if (b[108] != 9U || shift > 16U) {
    return -ENOTSUP;
  }
  if (length_hi != 0U || (uint64_t)base + length_lo > fs->dev_sectors || heap >= length_lo) {
    return -ENOENT;
  }

  fs->type = FAT_RO_EXFAT;
  fs->fat_sector = base + prv_get32(&b[80]);
  fs->data_sector = base + heap;
  fs->cluster_shift = shift;
  fs->num_clusters = prv_get32(&b[92]);
  fs->root_cluster = prv_get32(&b[96]);
  fs->root_sector = 0U;
  fs->root_sectors = 0U;

  return 0;
}

static int prv_mount_vbr(struct fat_ro *fs, uint32_t base, const uint8_t *b) {
  if (!prv_has_signature(b)) {
    return -ENOENT;
  }
  if (prv_is_exfat(b)) {
    return prv_mount_exfat(fs, base, b);
  }
  if (prv_is_fat(b)) {
    return prv_mount_fat(fs, base, b);
  }

  return -ENOENT;
}

int fat_ro_mount(struct fat_ro *fs, fat_ro_read_fn read, void *ctx, uint32_t dev_sectors) {
  uint8_t *b = fs->data_buf;
  int ret;

  fs->read = read;
  fs->ctx = ctx;
  fs->dev_sectors = dev_sectors;
  fs->fat_cached = NO_SECTOR;

  ret = prv_read(fs, 0U, b, 1U);
  if (ret != 0) {
    return ret;
  }
  if (!prv_has_signature(b)) {
    return -ENOENT;
  }

  // A volume of the whole device, or the first partition of its MBR
  if (prv_is_exfat(b) || prv_is_fat(b)) {
    return prv_mount_vbr(fs, 0U, b);
  }

  for (uint32_t i = 0U; i < 4U; i++) {
    const uint8_t *entry = &b[446U + 16U * i];
    const uint32_t start = prv_get32(&entry[8]);

    if (entry[4] == 0U || start == 0U) {
      continue;
    }

    ret = prv_read(fs, start, b, 1U);
    if (ret != 0) {
      return ret;
    }

    return prv_mount_vbr(fs, start, b);
  }

  return -ENOENT;
}

static bool prv_valid_cluster(const struct fat_ro *fs, uint32_t cluster) {
  return cluster >= 2U && cluster - 2U < fs->num_clusters;
}

// The cluster after @p cluster: 0, CHAIN_END at the end of the chain, or a negative errno
static int prv_next_cluster(struct fat_ro *fs, uint32_t cluster, uint32_t *next) {
  const uint32_t width = fs->type == FAT_RO_FAT16 ? 2U : 4U;
  const uint32_t offset = cluster * width;
  const uint32_t sector = fs->fat_sector + offset / SS;
  const uint8_t *p = &fs->fat_buf[offset % SS];
  uint32_t value;
  int ret;

  if (sector != fs->fat_cached) {
    fs->fat_cached = NO_SECTOR;
    ret = prv_read(fs, sector, fs->fat_buf, 1U);
    if (ret != 0) {
      return ret;
    }
    fs->fat_cached = sector;
  }

  switch (fs->type) {
    case FAT_RO_FAT16:
      value = prv_get16(p);
      if (value >= 0xFFF8U) {
        return CHAIN_END;
      }
      break;
    case FAT_RO_FAT32:
      value = prv_get32(p) & 0x0FFFFFFFU;
      if (value >= 0x0FFFFFF8U) {
        return CHAIN_END;
      }
      break;
    default:
      value = prv_get32(p);
      if (value == 0xFFFFFFFFU) {
        return CHAIN_END;
      }
      break;
  }

  if (!prv_valid_cluster(fs, value)) {
    return -EIO;
  }
  *next = value;

  return 0;
}

static uint32_t prv_cluster_sector(const struct fat_ro *fs, uint32_t cluster) {
  return fs->data_sector + ((cluster - 2U) << fs->cluster_shift);
}

struct dir_walk {
  fat_ro_file_cb cb;
  void *arg;
  bool end;
  // exFAT: a file entry waiting for its stream extension
  bool pending;
  bool pending_dir;
};

static int prv_file_found(struct fat_ro *fs, struct dir_walk *walk, uint32_t size,
                          uint32_t first_cluster, bool contiguous) {
  struct fat_ro_file file = {
    .size = size,
    .first_cluster = first_cluster,
    .contiguous = contiguous,
    .pos_index = 0U,
    .pos_cluster = first_cluster,
  };

  if (size != 0U && !prv_valid_cluster(fs, first_cluster)) {
    return 0;
  }

  return walk->cb(fs, &file, walk->arg);
}

static int prv_fat_entry(struct fat_ro *fs, struct dir_walk *walk, const uint8_t *e) {
  const uint8_t attr = e[11];
  uint32_t first;

  if (e[0] == 0U) {
    walk->end = true;
    return 0;
  }
  if (e[0] == FAT_ENTRY_FREE || (attr & FAT_ATTR_LFN) == FAT_ATTR_LFN ||
      (attr & (FAT_ATTR_LABEL | FAT_ATTR_DIR)) != 0U) {
    return 0;
  }

  first = prv_get16(&e[26]);
  if (fs->type == FAT_RO_FAT32) {
    first |= (uint32_t)prv_get16(&e[20]) << 16;
  }

  return prv_file_found(fs, walk, prv_get32(&e[28]), first, false);
}

static int prv_exfat_entry(struct fat_ro *fs, struct dir_walk *walk, const uint8_t *e) {
  const uint8_t type = e[0];

  if (type == 0U) {
    walk->end = true;
    return 0;
  }

  if (type == EXFAT_ENTRY_FILE) {
    walk->pending = e[1] != 0U;
    walk->pending_dir = (prv_get16(&e[4]) & FAT_ATTR_DIR) != 0U;
    return 0;
  }

  if (type == EXFAT_ENTRY_STREAM && walk->pending) {
    walk->pending = false;
    // Files of 4 GiB and more are left out
    if (walk->pending_dir || prv_get32(&e[28]) != 0U) {
      return 0;
    }
    return prv_file_found(fs, walk, prv_get32(&e[24]), prv_get32(&e[20]),
                          (e[1] & EXFAT_NO_FAT_CHAIN) != 0U);
  }

  // Another primary entry, or one not in use, ends the set
  if ((type & EXFAT_SECONDARY) == 0U || (type & 0x80U) == 0U) {
    walk->pending = false;
  }

  return 0;
}

static int prv_dir_sector(struct fat_ro *fs, struct dir_walk *walk, uint32_t sector) {
  int ret;

  ret = prv_read(fs, sector, fs->dir_buf, 1U);
  if (ret != 0) {
    return ret;
  }

  for (uint32_t off = 0U; off < SS && !walk->end; off += DIR_ENTRY_SIZE) {
    const uint8_t *e = &fs->dir_buf[off];

    ret = fs->type == FAT_RO_EXFAT ? prv_exfat_entry(fs, walk, e) : prv_fat_entry(fs, walk, e);
    if (ret != 0) {
      return ret;
    }
  }

  return 0;
}

int fat_ro_foreach_root_file(struct fat_ro *fs, fat_ro_file_cb cb, void *arg) {
  struct dir_walk walk = {.cb = cb, .arg = arg};
  uint32_t cluster = fs->root_cluster;
  int ret;

  if (fs->type == FAT_RO_FAT16) {
    for (uint32_t i = 0U; i < fs->root_sectors && !walk.end; i++) {
      ret = prv_dir_sector(fs, &walk, fs->root_sector + i);
      if (ret != 0) {
        return ret;
      }
    }
    return 0;
  }

  if (!prv_valid_cluster(fs, cluster)) {
    return -EIO;
  }

  // A chain longer than the volume loops
  for (uint32_t n = 0U; n < fs->num_clusters && !walk.end; n++) {
    for (uint32_t i = 0U; i < (1U << fs->cluster_shift) && !walk.end; i++) {
      ret = prv_dir_sector(fs, &walk, prv_cluster_sector(fs, cluster) + i);
      if (ret != 0) {
        return ret;
      }
    }
    if (walk.end) {
      break;
    }

    ret = prv_next_cluster(fs, cluster, &cluster);
    if (ret == CHAIN_END) {
      return 0;
    }
    if (ret != 0) {
      return ret;
    }
  }

  return walk.end ? 0 : -EIO;
}

// The cluster holding cluster @p index of the file
static int prv_file_cluster(struct fat_ro *fs, struct fat_ro_file *file, uint32_t index,
                            uint32_t *cluster) {
  int ret;

  if (file->contiguous) {
    if (index >= fs->num_clusters || !prv_valid_cluster(fs, file->first_cluster + index)) {
      return -EIO;
    }
    *cluster = file->first_cluster + index;
    return 0;
  }

  if (index < file->pos_index) {
    file->pos_index = 0U;
    file->pos_cluster = file->first_cluster;
  }
  while (file->pos_index < index) {
    ret = prv_next_cluster(fs, file->pos_cluster, &file->pos_cluster);
    if (ret != 0) {
      return ret == CHAIN_END ? -EIO : ret;
    }
    file->pos_index++;
  }
  *cluster = file->pos_cluster;

  return 0;
}

int fat_ro_read(struct fat_ro *fs, struct fat_ro_file *file, uint32_t offset, void *buf,
                size_t len) {
  const uint32_t cluster_bytes = SS << fs->cluster_shift;
  uint8_t *dst = buf;
  int ret;

  if (offset > file->size || len > file->size - offset) {
    return -EINVAL;
  }

  while (len > 0U) {
    const uint32_t in_cluster = offset & (cluster_bytes - 1U);
    const uint32_t in_sector = offset % SS;
    uint32_t cluster;
    uint32_t sector;
    size_t n;

    ret = prv_file_cluster(fs, file, offset >> (fs->cluster_shift + 9U), &cluster);
    if (ret != 0) {
      return ret;
    }
    sector = prv_cluster_sector(fs, cluster) + in_cluster / SS;

    if (in_sector == 0U && len >= SS) {
      // Whole sectors, up to the end of the cluster, straight into the buffer
      uint32_t count = (cluster_bytes - in_cluster) / SS;

      if (count > len / SS) {
        count = len / SS;
      }
      ret = prv_read(fs, sector, dst, count);
      if (ret != 0) {
        return ret;
      }
      n = count * SS;
    } else {
      ret = prv_read(fs, sector, fs->data_buf, 1U);
      if (ret != 0) {
        return ret;
      }
      n = SS - in_sector;
      if (n > len) {
        n = len;
      }
      memcpy(dst, &fs->data_buf[in_sector], n);
    }

    offset += n;
    dst += n;
    len -= n;
  }

  return 0;
}
