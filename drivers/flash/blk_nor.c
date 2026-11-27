/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "blk_nor.h"

#include <errno.h>
#include <string.h>

#define BS BLK_NOR_BLOCK_SIZE

static int prv_find(const struct blk_nor *nor, uint32_t block) {
  for (int i = 0; i < (int)BLK_NOR_CACHE_BLOCKS; i++) {
    if (nor->entries[i].block == block) {
      return i;
    }
  }

  return -1;
}

static void prv_touch(struct blk_nor *nor, int i) {
  nor->entries[i].last_use = ++nor->clock;
}

// The entry of @p block, read from the device unless already cached, evicting the least recently
// used one
static int prv_load(struct blk_nor *nor, uint32_t block, int *idx) {
  int i = prv_find(nor, block);
  int ret;

  if (i < 0) {
    i = 0;
    for (int j = 0; j < (int)BLK_NOR_CACHE_BLOCKS; j++) {
      if (nor->entries[j].block == BLK_NOR_NO_BLOCK) {
        i = j;
        break;
      }
      if (nor->entries[j].last_use < nor->entries[i].last_use) {
        i = j;
      }
    }

    nor->entries[i].block = BLK_NOR_NO_BLOCK;
    ret = nor->hw->read(nor->ctx, block, nor->cache[i], 1U);
    if (ret != 0) {
      return ret;
    }
    nor->entries[i].block = block;
  }

  prv_touch(nor, i);
  *idx = i;

  return 0;
}

// Blocks of the run [@p block, @p block + @p count) the cache holds now hold @p data, or are
// forgotten without it
static void prv_update(struct blk_nor *nor, uint32_t block, uint32_t count, const uint8_t *data) {
  for (int i = 0; i < (int)BLK_NOR_CACHE_BLOCKS; i++) {
    const uint32_t b = nor->entries[i].block;

    if (b == BLK_NOR_NO_BLOCK || b < block || b - block >= count) {
      continue;
    }
    if (data != nullptr) {
      memcpy(nor->cache[i], &data[(b - block) * BS], BS);
    } else {
      nor->entries[i].block = BLK_NOR_NO_BLOCK;
    }
  }
}

void blk_nor_init(struct blk_nor *nor, const struct blk_nor_hw *hw, void *ctx) {
  nor->hw = hw;
  nor->ctx = ctx;
  nor->clock = 0U;
  memset(nor->erased, 0xFF, sizeof(nor->erased));
  blk_nor_invalidate(nor);
}

void blk_nor_invalidate(struct blk_nor *nor) {
  for (uint32_t i = 0U; i < BLK_NOR_CACHE_BLOCKS; i++) {
    nor->entries[i].block = BLK_NOR_NO_BLOCK;
    nor->entries[i].last_use = 0U;
  }
}

int blk_nor_read(struct blk_nor *nor, uint32_t addr, void *buf, size_t len) {
  uint8_t *dst = buf;
  int ret;

  while (len > 0U) {
    const uint32_t block = addr / BS;
    const uint32_t off = addr % BS;
    size_t n = BS - off;
    int i = prv_find(nor, block);

    if (i < 0 && off == 0U && len >= BS) {
      // Whole blocks the cache does not hold, straight from the device
      uint32_t count = 1U;

      while (count < len / BS && prv_find(nor, block + count) < 0) {
        count++;
      }
      ret = nor->hw->read(nor->ctx, block, dst, count);
      if (ret != 0) {
        return ret;
      }
      n = count * BS;
    } else {
      if (i < 0) {
        ret = prv_load(nor, block, &i);
        if (ret != 0) {
          return ret;
        }
      } else {
        prv_touch(nor, i);
      }
      if (n > len) {
        n = len;
      }
      memcpy(dst, &nor->cache[i][off], n);
    }

    addr += n;
    dst += n;
    len -= n;
  }

  return 0;
}

// ANDs @p src into @p dst, telling whether that changed anything
static bool prv_and(uint8_t *dst, const uint8_t *src, size_t len) {
  bool changed = false;

  for (size_t i = 0U; i < len; i++) {
    const uint8_t v = dst[i] & src[i];

    changed |= (v != dst[i]);
    dst[i] = v;
  }

  return changed;
}

int blk_nor_write(struct blk_nor *nor, uint32_t addr, const void *buf, size_t len) {
  const uint8_t *src = buf;
  int ret;

  while (len > 0U) {
    const uint32_t block = addr / BS;
    const uint32_t off = addr % BS;
    size_t n;

    if (off == 0U && len >= BS) {
      uint32_t count = len / BS;

      if (count > BLK_NOR_BATCH_BLOCKS) {
        count = BLK_NOR_BATCH_BLOCKS;
      }
      n = count * BS;

      // The cache holds what the device does: reading the device is as good
      ret = nor->hw->read(nor->ctx, block, nor->batch, count);
      if (ret != 0) {
        return ret;
      }
      if (prv_and(nor->batch, src, n)) {
        ret = nor->hw->write(nor->ctx, block, nor->batch, count);
        if (ret != 0) {
          prv_update(nor, block, count, nullptr);
          return ret;
        }
        prv_update(nor, block, count, nor->batch);
      }
    } else {
      int i;

      n = BS - off;
      if (n > len) {
        n = len;
      }

      ret = prv_load(nor, block, &i);
      if (ret != 0) {
        return ret;
      }
      if (prv_and(&nor->cache[i][off], src, n)) {
        ret = nor->hw->write(nor->ctx, block, nor->cache[i], 1U);
        if (ret != 0) {
          nor->entries[i].block = BLK_NOR_NO_BLOCK;
          return ret;
        }
      }
    }

    addr += n;
    src += n;
    len -= n;
  }

  return 0;
}

int blk_nor_erase(struct blk_nor *nor, uint32_t addr, size_t len) {
  uint32_t block = addr / BS;
  uint32_t count = len / BS;
  int ret;

  if ((addr % BS) != 0U || (len % BS) != 0U) {
    return -EINVAL;
  }

  while (count > 0U) {
    const uint32_t n = count < BLK_NOR_BATCH_BLOCKS ? count : BLK_NOR_BATCH_BLOCKS;

    ret = nor->hw->write(nor->ctx, block, nor->erased, n);
    if (ret != 0) {
      prv_update(nor, block, n, nullptr);
      return ret;
    }
    prv_update(nor, block, n, nor->erased);

    block += n;
    count -= n;
  }

  return 0;
}
