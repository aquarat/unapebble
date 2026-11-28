/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// A FAT16 volume computed sector by sector (Microsoft's FAT specification, "FAT: General
// Overview of On-Disk Format", 1.03, for the boot sector, the FAT and the directory entries, long
// names included).

#include <errno.h>
#include <string.h>

#include <pbl/vfat/vfat.h>

#define SS VFAT_SECTOR_SIZE

#define DEFAULT_SECTORS 262144U
#define DEFAULT_SPC     8U

#define DIR_ENTRY_SIZE   32U
#define ENTRIES_A_SECTOR (SS / DIR_ENTRY_SIZE)
#define ROOT_SECTORS     (VFAT_ROOT_ENTRIES / ENTRIES_A_SECTOR)
#define NUM_FATS         2U
#define FAT16_ENTRIES    (SS / 2U)

// FAT16 has at least this many clusters, and fewer than the next
#define FAT16_MIN_CLUSTERS 4085U
#define FAT16_MAX_CLUSTERS 65524U

#define MEDIA        0xF8U
#define FAT16_EOC    0xFFFFU
#define ATTR_LABEL   0x08U
#define ATTR_LFN     0x0FU
#define LFN_LAST     0x40U
#define LFN_CHARS    13U
#define SFN_BASE_LEN 8U
#define SFN_EXT_LEN  3U

static size_t prv_strnlen(const char *s, size_t max) {
  size_t n = 0U;

  while (n < max && s[n] != '\0') {
    n++;
  }

  return n;
}

static void prv_put16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void prv_put32(uint8_t *p, uint32_t v) {
  prv_put16(p, v);
  prv_put16(&p[2], v >> 16);
}

uint16_t vfat_date(unsigned int year, unsigned int month, unsigned int day) {
  if (year < 1980U || month < 1U || month > 12U || day < 1U || day > 31U) {
    return (uint16_t)((1U << 5) | 1U);
  }
  if (year > 2107U) {
    year = 2107U;
  }

  return (uint16_t)(((year - 1980U) << 9) | (month << 5) | day);
}

uint16_t vfat_time(unsigned int hour, unsigned int minute, unsigned int second) {
  return (uint16_t)(((hour % 24U) << 11) | ((minute % 60U) << 5) | ((second % 60U) / 2U));
}

static bool prv_sfn_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

static bool prv_name_char(char c) {
  return c >= 0x20 && c <= 0x7e && strchr("/\\:*?\"<>|", c) == NULL;
}

static char prv_upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// A name that is its own 8.3 name: upper case, a base of 1 to 8 characters, an extension of up to
// 3 after a dot
static bool prv_is_83(const char *name, char sfn[11]) {
  const char *dot = strchr(name, '.');
  const size_t len = strlen(name);
  const size_t base = dot != NULL ? (size_t)(dot - name) : len;
  const size_t ext = dot != NULL ? len - base - 1U : 0U;

  if (base == 0U || base > SFN_BASE_LEN || ext > SFN_EXT_LEN || (dot != NULL && ext == 0U) ||
      (dot != NULL && strchr(dot + 1, '.') != NULL)) {
    return false;
  }
  for (size_t i = 0U; i < len; i++) {
    if (&name[i] != dot && !prv_sfn_char(name[i])) {
      return false;
    }
  }

  memset(sfn, ' ', 11);
  memcpy(sfn, name, base);
  if (ext != 0U) {
    memcpy(&sfn[SFN_BASE_LEN], dot + 1, ext);
  }

  return true;
}

// Characters of a long name that can go in its alias, upper case
static size_t prv_alias_part(const char *s, size_t len, char *out, size_t max) {
  size_t n = 0U;

  for (size_t i = 0U; i < len && n < max; i++) {
    const char c = prv_upper(s[i]);

    if (c == ' ' || c == '.') {
      continue;
    }
    out[n++] = prv_sfn_char(c) ? c : '_';
  }

  return n;
}

static bool prv_sfn_used(const struct vfat_file *files, size_t n, const char sfn[11]) {
  for (size_t i = 0U; i < n; i++) {
    if (memcmp(files[i].sfn, sfn, 11) == 0) {
      return true;
    }
  }

  return false;
}

// The 8.3 alias of a long name: what can be kept of its first characters, "~" and the first number
// no other file has, and its extension
static int prv_alias(const struct vfat_file *files, size_t n, const char *name, char sfn[11]) {
  const char *dot = strrchr(name, '.');
  const size_t len = strlen(name);
  const size_t base_len = dot != NULL && dot != name ? (size_t)(dot - name) : len;
  char base[SFN_BASE_LEN];
  char ext[SFN_EXT_LEN];
  const size_t nb = prv_alias_part(name, base_len, base, sizeof(base));
  const size_t ne = base_len < len ? prv_alias_part(dot + 1, len - base_len - 1U, ext, 3U) : 0U;

  for (unsigned int num = 1U; num < 1000000U; num++) {
    char tail[8];
    size_t tl = 0U;
    size_t keep;

    for (unsigned int v = num; v != 0U; v /= 10U) {
      tail[tl++] = (char)('0' + v % 10U);
    }
    tail[tl++] = '~';
    keep = nb < SFN_BASE_LEN - tl ? nb : SFN_BASE_LEN - tl;

    memset(sfn, ' ', 11);
    memcpy(sfn, base, keep);
    for (size_t i = 0U; i < tl; i++) {
      sfn[keep + i] = tail[tl - 1U - i];
    }
    memcpy(&sfn[SFN_BASE_LEN], ext, ne);
    if (!prv_sfn_used(files, n, sfn)) {
      return 0;
    }
  }

  return -EINVAL;
}

static uint8_t prv_sfn_checksum(const char sfn[11]) {
  uint8_t sum = 0U;

  for (size_t i = 0U; i < 11U; i++) {
    sum = (uint8_t)(((sum & 1U) << 7) + (sum >> 1) + (uint8_t)sfn[i]);
  }

  return sum;
}

static int prv_check_name(const char *name) {
  const size_t len = prv_strnlen(name, VFAT_NAME_MAX + 1U);

  if (len == 0U || len > VFAT_NAME_MAX || name[0] == ' ' || name[len - 1U] == ' ' ||
      name[len - 1U] == '.') {
    return -EINVAL;
  }
  for (size_t i = 0U; i < len; i++) {
    if (!prv_name_char(name[i])) {
      return -EINVAL;
    }
  }

  return 0;
}

// FAT sectors for the clusters the rest of the volume leaves, with the reserved sectors that
// align the data on a cluster
static int prv_geometry(struct vfat *v) {
  const uint32_t total = v->p.total_sectors;
  const uint32_t spc = v->p.sectors_per_cluster;
  uint32_t fat = 1U;
  uint32_t reserved = 1U;
  uint32_t clusters = 0U;

  for (int i = 0; i < 8; i++) {
    const uint32_t meta = reserved + NUM_FATS * fat + ROOT_SECTORS;

    if (meta >= total) {
      return -EINVAL;
    }
    clusters = (total - meta) / spc;
    fat = ((clusters + 2U) * 2U + SS - 1U) / SS;
  }
  reserved += (spc - (reserved + NUM_FATS * fat + ROOT_SECTORS) % spc) % spc;
  if (reserved + NUM_FATS * fat + ROOT_SECTORS >= total) {
    return -EINVAL;
  }
  clusters = (total - reserved - NUM_FATS * fat - ROOT_SECTORS) / spc;
  if (clusters < FAT16_MIN_CLUSTERS || clusters > FAT16_MAX_CLUSTERS ||
      (clusters + 2U) * 2U > fat * SS) {
    return -EINVAL;
  }

  v->reserved = reserved;
  v->fat_sectors = fat;
  v->root_sector = reserved + NUM_FATS * fat;
  v->data_sector = v->root_sector + ROOT_SECTORS;
  v->num_clusters = clusters;

  return 0;
}

int vfat_layout(struct vfat *v, const struct vfat_params *p, struct vfat_file *files, size_t num) {
  const uint32_t cluster_bytes =
      SS * (p->sectors_per_cluster != 0U ? p->sectors_per_cluster : DEFAULT_SPC);
  uint32_t next = 2U;
  uint16_t entry = 1U;
  size_t n;
  int ret;

  *v = (struct vfat){.p = *p, .files = files};
  if (v->p.total_sectors == 0U) {
    v->p.total_sectors = DEFAULT_SECTORS;
  }
  if (v->p.sectors_per_cluster == 0U) {
    v->p.sectors_per_cluster = DEFAULT_SPC;
  }
  if ((v->p.sectors_per_cluster & (v->p.sectors_per_cluster - 1U)) != 0U ||
      v->p.sectors_per_cluster > 128U) {
    return -EINVAL;
  }

  ret = prv_geometry(v);
  if (ret != 0) {
    return ret;
  }

  for (n = 0U; n < num; n++) {
    struct vfat_file *f = &files[n];
    const uint32_t clusters = (f->size + cluster_bytes - 1U) / cluster_bytes;
    size_t len;

    ret = prv_check_name(f->name);
    if (ret != 0) {
      return ret;
    }
    len = strlen(f->name);

    if (prv_is_83(f->name, f->sfn) && !prv_sfn_used(files, n, f->sfn)) {
      f->num_lfn = 0U;
    } else {
      ret = prv_alias(files, n, f->name, f->sfn);
      if (ret != 0) {
        return ret;
      }
      f->num_lfn = (uint8_t)((len + LFN_CHARS - 1U) / LFN_CHARS);
    }

    if (entry + f->num_lfn + 1U > VFAT_ROOT_ENTRIES || clusters > v->num_clusters + 2U - next) {
      break;
    }
    f->first_entry = entry;
    entry += f->num_lfn + 1U;
    f->first_cluster = next;
    f->num_clusters = clusters;
    next += clusters;
  }

  v->num_files = n;
  v->num_entries = entry;

  return (int)n;
}

uint32_t vfat_num_sectors(const struct vfat *v) {
  return v->p.total_sectors;
}

static void prv_boot_sector(const struct vfat *v, uint8_t *b) {
  static const uint8_t s_jump[3] = {0xEBU, 0x3CU, 0x90U};

  memset(b, 0, SS);
  memcpy(b, s_jump, sizeof(s_jump));
  memcpy(&b[3], "MSWIN4.1", 8);
  prv_put16(&b[11], SS);
  b[13] = v->p.sectors_per_cluster;
  prv_put16(&b[14], v->reserved);
  b[16] = NUM_FATS;
  prv_put16(&b[17], VFAT_ROOT_ENTRIES);
  if (v->p.total_sectors < 0x10000U) {
    prv_put16(&b[19], v->p.total_sectors);
  } else {
    prv_put32(&b[32], v->p.total_sectors);
  }
  b[21] = MEDIA;
  prv_put16(&b[22], v->fat_sectors);
  prv_put16(&b[24], 63U);
  prv_put16(&b[26], 255U);
  b[36] = 0x80U;
  b[38] = 0x29U;
  prv_put32(&b[39], v->p.serial);
  memset(&b[43], ' ', 11);
  if (v->p.label != NULL) {
    memcpy(&b[43], v->p.label, prv_strnlen(v->p.label, 11));
  } else {
    memcpy(&b[43], "NO NAME", 7);
  }
  memcpy(&b[54], "FAT16   ", 8);
  // A boot sector started anyway halts
  b[62] = 0xF4U;
  b[63] = 0xEBU;
  b[64] = 0xFDU;
  b[510] = 0x55U;
  b[511] = 0xAAU;
}

// The first file that ends after cluster c: files take their clusters in order, an empty one
// none where the next would start
static size_t prv_file_by_cluster(const struct vfat *v, uint32_t c) {
  size_t lo = 0U;
  size_t hi = v->num_files;

  while (lo < hi) {
    const size_t mid = (lo + hi) / 2U;
    const struct vfat_file *f = &v->files[mid];

    if (f->first_cluster + f->num_clusters <= c) {
      lo = mid + 1U;
    } else {
      hi = mid;
    }
  }

  return lo;
}

static void prv_fat_sector(const struct vfat *v, uint32_t s, uint8_t *b) {
  const uint32_t first = s * FAT16_ENTRIES;
  size_t i = prv_file_by_cluster(v, first);

  memset(b, 0, SS);
  for (uint32_t k = 0U; k < FAT16_ENTRIES; k++) {
    const uint32_t c = first + k;
    uint32_t value = 0U;

    if (c == 0U) {
      value = 0xFF00U | MEDIA;
    } else if (c == 1U) {
      value = FAT16_EOC;
    } else {
      while (i < v->num_files && v->files[i].first_cluster + v->files[i].num_clusters <= c) {
        i++;
      }
      if (i == v->num_files) {
        break;
      }
      if (c >= v->files[i].first_cluster) {
        value = c + 1U == v->files[i].first_cluster + v->files[i].num_clusters ? FAT16_EOC : c + 1U;
      }
    }
    prv_put16(&b[2U * k], value);
  }
}

// The file a directory entry belongs to
static const struct vfat_file *prv_file_by_entry(const struct vfat *v, uint32_t e) {
  size_t lo = 0U;
  size_t hi = v->num_files;

  while (lo < hi) {
    const size_t mid = (lo + hi) / 2U;
    const struct vfat_file *f = &v->files[mid];

    if (e < f->first_entry) {
      hi = mid;
    } else if (e > f->first_entry + f->num_lfn) {
      lo = mid + 1U;
    } else {
      return f;
    }
  }

  return NULL;
}

static void prv_lfn_entry(const struct vfat_file *f, uint32_t j, uint8_t *d) {
  static const uint8_t s_pos[LFN_CHARS] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  const uint32_t ord = f->num_lfn - j;
  const size_t len = strlen(f->name);

  d[0] = (uint8_t)(ord | (j == 0U ? LFN_LAST : 0U));
  d[11] = ATTR_LFN;
  d[13] = prv_sfn_checksum(f->sfn);
  for (uint32_t k = 0U; k < LFN_CHARS; k++) {
    const size_t c = (ord - 1U) * LFN_CHARS + k;
    const uint32_t ch = c < len ? (uint8_t)f->name[c] : (c == len ? 0x0000U : 0xFFFFU);

    prv_put16(&d[s_pos[k]], ch);
  }
}

static void prv_sfn_entry(const struct vfat_file *f, uint8_t *d) {
  memcpy(d, f->sfn, 11);
  prv_put16(&d[14], f->time);
  prv_put16(&d[16], f->date);
  prv_put16(&d[18], f->date);
  prv_put16(&d[22], f->time);
  prv_put16(&d[24], f->date);
  prv_put16(&d[26], f->num_clusters != 0U ? f->first_cluster : 0U);
  prv_put32(&d[28], f->size);
}

static void prv_dir_sector(const struct vfat *v, uint32_t s, uint8_t *b) {
  memset(b, 0, SS);
  for (uint32_t k = 0U; k < ENTRIES_A_SECTOR; k++) {
    const uint32_t e = s * ENTRIES_A_SECTOR + k;
    uint8_t *d = &b[k * DIR_ENTRY_SIZE];
    const struct vfat_file *f;

    if (e >= v->num_entries) {
      break;
    }
    if (e == 0U) {
      memset(d, ' ', 11);
      if (v->p.label != NULL) {
        memcpy(d, v->p.label, prv_strnlen(v->p.label, 11));
      }
      d[11] = ATTR_LABEL;
      prv_put16(&d[22], v->p.time);
      prv_put16(&d[24], v->p.date);
      continue;
    }
    f = prv_file_by_entry(v, e);
    if (f == NULL) {
      continue;
    }
    if (e - f->first_entry < f->num_lfn) {
      prv_lfn_entry(f, e - f->first_entry, d);
    } else {
      prv_sfn_entry(f, d);
    }
  }
}

int vfat_file_at(const struct vfat *v, uint32_t lba) {
  uint32_t c;
  size_t i;

  if (lba < v->data_sector || lba >= v->p.total_sectors) {
    return -1;
  }
  c = 2U + (lba - v->data_sector) / v->p.sectors_per_cluster;
  i = prv_file_by_cluster(v, c);

  return (i < v->num_files && c >= v->files[i].first_cluster) ? (int)i : -1;
}

// Sectors of file data from lba, at most count, in one read: what the file has there, then zeros
static int prv_data(const struct vfat *v, uint32_t lba, uint8_t *b, uint32_t count,
                    uint32_t *done) {
  const int i = vfat_file_at(v, lba);
  const struct vfat_file *f;
  uint32_t offset;
  uint32_t left;
  uint32_t n;
  uint32_t bytes;
  int ret;

  if (i < 0) {
    memset(b, 0, SS);
    *done = 1U;
    return 0;
  }

  f = &v->files[i];
  offset = (lba - v->data_sector - (f->first_cluster - 2U) * v->p.sectors_per_cluster) * SS;
  if (offset >= f->size) {
    memset(b, 0, SS);
    *done = 1U;
    return 0;
  }

  left = f->size - offset;
  n = (left + SS - 1U) / SS;
  if (n > count) {
    n = count;
  }
  bytes = n * SS < left ? n * SS : left;
  ret = v->p.read(v->p.ctx, (size_t)i, offset, b, bytes);
  if (ret != 0) {
    return ret;
  }
  memset(&b[bytes], 0, n * SS - bytes);
  *done = n;

  return 0;
}

int vfat_read(const struct vfat *v, uint32_t lba, void *buf, uint32_t count) {
  uint8_t *b = buf;
  const uint32_t fat_end = v->reserved + NUM_FATS * v->fat_sectors;

  if (lba >= v->p.total_sectors || count > v->p.total_sectors - lba) {
    return -EINVAL;
  }

  while (count > 0U) {
    uint32_t done = 1U;
    int ret = 0;

    if (lba == 0U) {
      prv_boot_sector(v, b);
    } else if (lba < v->reserved) {
      memset(b, 0, SS);
    } else if (lba < fat_end) {
      prv_fat_sector(v, (lba - v->reserved) % v->fat_sectors, b);
    } else if (lba < v->data_sector) {
      prv_dir_sector(v, lba - v->root_sector, b);
    } else {
      ret = prv_data(v, lba, b, count, &done);
    }
    if (ret != 0) {
      return ret;
    }
    lba += done;
    b += done * SS;
    count -= done;
  }

  return 0;
}
