/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "ecc_nor.h"

#include <string.h>

#define ERASED_BYTE 0xFFU

static bool prv_test(const uint32_t *bits, uint32_t i) {
  return (bits[i / 32U] & (1UL << (i % 32U))) != 0U;
}

static void prv_set(uint32_t *bits, uint32_t i) {
  bits[i / 32U] |= 1UL << (i % 32U);
}

static void prv_clear(uint32_t *bits, uint32_t i) {
  bits[i / 32U] &= ~(1UL << (i % 32U));
}

static bool prv_erased(const uint8_t *p, size_t len) {
  for (size_t i = 0U; i < len; i++) {
    if (p[i] != ERASED_BYTE) {
      return false;
    }
  }

  return true;
}

static bool prv_any_pending(const struct ecc_nor_slot *slot) {
  for (size_t i = 0U; i < (ECC_NOR_PAGE_WORDS / 32U); i++) {
    if (slot->pending[i] != 0U) {
      return true;
    }
  }

  return false;
}

static bool prv_complete(const struct ecc_nor_slot *slot, uint32_t w) {
  const uint8_t *written = &slot->written[w * (ECC_NOR_WORD_SIZE / 8U)];

  return written[0] == 0xFFU && written[1] == 0xFFU;
}

static uint32_t prv_hold_ms(const struct ecc_nor_slot *slot, uint32_t w) {
  return prv_complete(slot, w) ? ECC_NOR_SETTLE_MS : ECC_NOR_PARTIAL_MS;
}

static bool prv_due(const struct ecc_nor *nor, const struct ecc_nor_slot *slot, uint32_t w,
                    uint32_t now_ms, bool all) {
  return all || nor->write_through || (now_ms - slot->since_ms[w]) >= prv_hold_ms(slot, w);
}

static void prv_word_done(struct ecc_nor_slot *slot, uint32_t w) {
  prv_clear(slot->pending, w);
  memset(&slot->written[w * (ECC_NOR_WORD_SIZE / 8U)], 0, ECC_NOR_WORD_SIZE / 8U);
}

static void prv_sync(struct ecc_nor *nor) {
  if (nor->changed && nor->hw->sync != nullptr) {
    nor->hw->sync(nor->ctx);
  }
  nor->changed = false;
}

static int prv_erase_page(struct ecc_nor *nor, uint32_t page) {
  nor->changed = true;

  return nor->hw->erase_page(nor->ctx, page);
}

static int prv_program(struct ecc_nor *nor, struct ecc_nor_slot *slot, uint32_t w) {
  const uint8_t *data = &slot->data[w * ECC_NOR_WORD_SIZE];
  int ret = 0;

  if (!prv_erased(data, ECC_NOR_WORD_SIZE)) {
    nor->changed = true;
    ret = nor->hw->program(nor->ctx, slot->page + (w * ECC_NOR_WORD_SIZE), data);
  }
  if (ret == 0) {
    prv_word_done(slot, w);
  }

  return ret;
}

// Erases the page and programs it again: every word the flash held, and the pending words that
// are due; the others stay pending, on an erased page
static int prv_rewrite(struct ecc_nor *nor, struct ecc_nor_slot *slot, uint32_t now_ms,
                       bool all) {
  uint32_t in_flash[ECC_NOR_PAGE_WORDS / 32U] = {};
  uint8_t word[ECC_NOR_WORD_SIZE];
  int ret;

  for (uint32_t w = 0U; w < ECC_NOR_PAGE_WORDS; w++) {
    nor->hw->read(nor->ctx, slot->page + (w * ECC_NOR_WORD_SIZE), word, sizeof(word));
    if (!prv_erased(word, sizeof(word))) {
      prv_set(in_flash, w);
    }
  }

  ret = prv_erase_page(nor, slot->page);
  if (ret != 0) {
    return ret;
  }

  for (uint32_t w = 0U; w < ECC_NOR_PAGE_WORDS; w++) {
    if (prv_erased(&slot->data[w * ECC_NOR_WORD_SIZE], ECC_NOR_WORD_SIZE)) {
      prv_word_done(slot, w);
      continue;
    }

    if (prv_test(in_flash, w) || !prv_test(slot->pending, w) ||
        prv_due(nor, slot, w, now_ms, all)) {
      ret = prv_program(nor, slot, w);
      if (ret != 0) {
        return ret;
      }
    }
  }

  return 0;
}

static int prv_slot_flush(struct ecc_nor *nor, struct ecc_nor_slot *slot, uint32_t now_ms,
                          bool all) {
  uint8_t word[ECC_NOR_WORD_SIZE];
  bool rewrite = false;
  int ret;

  if (slot->page == ECC_NOR_NO_PAGE || !prv_any_pending(slot)) {
    return 0;
  }

  // A due word whose bits were cleared after it was programmed takes the whole page
  for (uint32_t w = 0U; w < ECC_NOR_PAGE_WORDS; w++) {
    if (!prv_test(slot->pending, w) || !prv_due(nor, slot, w, now_ms, all)) {
      continue;
    }

    nor->hw->read(nor->ctx, slot->page + (w * ECC_NOR_WORD_SIZE), word, sizeof(word));
    if (memcmp(word, &slot->data[w * ECC_NOR_WORD_SIZE], sizeof(word)) == 0) {
      prv_word_done(slot, w);
    } else if (!prv_erased(word, sizeof(word))) {
      rewrite = true;
      break;
    }
  }

  if (rewrite) {
    return prv_rewrite(nor, slot, now_ms, all);
  }

  for (uint32_t w = 0U; w < ECC_NOR_PAGE_WORDS; w++) {
    if (prv_test(slot->pending, w) && prv_due(nor, slot, w, now_ms, all)) {
      ret = prv_program(nor, slot, w);
      if (ret != 0) {
        return ret;
      }
    }
  }

  return 0;
}

static struct ecc_nor_slot *prv_slot_find(struct ecc_nor *nor, uint32_t page) {
  for (size_t i = 0U; i < ECC_NOR_SLOTS; i++) {
    if (nor->slots[i].page == page) {
      nor->slots[i].last_use = ++nor->clock;
      return &nor->slots[i];
    }
  }

  return nullptr;
}

// The cached page, else a free slot, else the least recently used one, preferring those
// without pending words, whose words go to the flash first
static int prv_slot_get(struct ecc_nor *nor, uint32_t page, struct ecc_nor_slot **out) {
  struct ecc_nor_slot *victim = nullptr;
  bool victim_pending = true;
  int ret;

  *out = prv_slot_find(nor, page);
  if (*out != nullptr) {
    return 0;
  }

  for (size_t i = 0U; i < ECC_NOR_SLOTS; i++) {
    struct ecc_nor_slot *slot = &nor->slots[i];
    bool pending;

    if (slot->page == ECC_NOR_NO_PAGE) {
      victim = slot;
      break;
    }

    pending = prv_any_pending(slot);
    if (victim == nullptr || (victim_pending && !pending) ||
        (victim_pending == pending && slot->last_use < victim->last_use)) {
      victim = slot;
      victim_pending = pending;
    }
  }

  ret = prv_slot_flush(nor, victim, 0U, true);
  if (ret != 0) {
    return ret;
  }
  prv_sync(nor);

  victim->page = page;
  victim->last_use = ++nor->clock;
  memset(victim->pending, 0, sizeof(victim->pending));
  memset(victim->written, 0, sizeof(victim->written));
  nor->hw->read(nor->ctx, page, victim->data, sizeof(victim->data));

  *out = victim;

  return 0;
}

void ecc_nor_init(struct ecc_nor *nor, const struct ecc_nor_hw *hw, void *ctx) {
  memset(nor, 0, sizeof(*nor));
  nor->hw = hw;
  nor->ctx = ctx;
  for (size_t i = 0U; i < ECC_NOR_SLOTS; i++) {
    nor->slots[i].page = ECC_NOR_NO_PAGE;
  }
}

void ecc_nor_read(struct ecc_nor *nor, uint32_t addr, void *buf, size_t len) {
  uint8_t *dst = buf;

  while (len > 0U) {
    const uint32_t page = addr & ~(ECC_NOR_PAGE_SIZE - 1U);
    const uint32_t offs = addr - page;
    const size_t chunk = (len < (ECC_NOR_PAGE_SIZE - offs)) ? len : (ECC_NOR_PAGE_SIZE - offs);
    const struct ecc_nor_slot *slot = prv_slot_find(nor, page);

    if (slot != nullptr) {
      memcpy(dst, &slot->data[offs], chunk);
    } else {
      nor->hw->read(nor->ctx, addr, dst, chunk);
    }

    addr += chunk;
    dst += chunk;
    len -= chunk;
  }
}

int ecc_nor_write(struct ecc_nor *nor, uint32_t addr, const void *buf, size_t len,
                  uint32_t now_ms) {
  const uint8_t *src = buf;
  int ret = 0;

  while (len > 0U) {
    const uint32_t page = addr & ~(ECC_NOR_PAGE_SIZE - 1U);
    const uint32_t offs = addr - page;
    const size_t chunk = (len < (ECC_NOR_PAGE_SIZE - offs)) ? len : (ECC_NOR_PAGE_SIZE - offs);
    struct ecc_nor_slot *slot;

    ret = prv_slot_get(nor, page, &slot);
    if (ret != 0) {
      break;
    }

    for (size_t i = 0U; i < chunk; i++) {
      const uint32_t b = offs + i;
      const uint32_t w = b / ECC_NOR_WORD_SIZE;
      const uint8_t old = slot->data[b];
      const uint8_t new = old & src[i];

      if (new != old) {
        slot->data[b] = new;
        if (!prv_test(slot->pending, w)) {
          prv_set(slot->pending, w);
          slot->since_ms[w] = now_ms;
        }
      }
      slot->written[b / 8U] |= (uint8_t)(1U << (b % 8U));
    }

    if (nor->write_through) {
      ret = prv_slot_flush(nor, slot, now_ms, true);
      if (ret != 0) {
        break;
      }
    }

    addr += chunk;
    src += chunk;
    len -= chunk;
  }

  prv_sync(nor);

  return ret;
}

int ecc_nor_erase(struct ecc_nor *nor, uint32_t addr, size_t len, uint32_t now_ms) {
  uint8_t word[ECC_NOR_WORD_SIZE];
  int ret = 0;

  while (len > 0U) {
    const uint32_t page = addr & ~(ECC_NOR_PAGE_SIZE - 1U);
    const uint32_t offs = addr - page;
    const size_t chunk = (len < (ECC_NOR_PAGE_SIZE - offs)) ? len : (ECC_NOR_PAGE_SIZE - offs);
    struct ecc_nor_slot *slot;
    bool in_flash = false;

    if (chunk == ECC_NOR_PAGE_SIZE) {
      slot = prv_slot_find(nor, page);
      if (slot != nullptr) {
        slot->page = ECC_NOR_NO_PAGE;
      }
      ret = prv_erase_page(nor, page);
    } else {
      ret = prv_slot_get(nor, page, &slot);
      if (ret != 0) {
        break;
      }

      for (uint32_t b = offs; b < offs + chunk; b += ECC_NOR_WORD_SIZE) {
        nor->hw->read(nor->ctx, page + b, word, sizeof(word));
        in_flash = in_flash || !prv_erased(word, sizeof(word));
        prv_word_done(slot, b / ECC_NOR_WORD_SIZE);
      }
      memset(&slot->data[offs], ERASED_BYTE, chunk);

      // The rest of the page goes back as it was
      if (in_flash) {
        ret = prv_rewrite(nor, slot, now_ms, false);
      }
    }

    if (ret != 0) {
      break;
    }

    addr += chunk;
    len -= chunk;
  }

  prv_sync(nor);

  return ret;
}

int ecc_nor_flush(struct ecc_nor *nor, uint32_t now_ms, bool all) {
  int ret = 0;

  for (size_t i = 0U; i < ECC_NOR_SLOTS && ret == 0; i++) {
    ret = prv_slot_flush(nor, &nor->slots[i], now_ms, all);
  }

  prv_sync(nor);

  return ret;
}

uint32_t ecc_nor_next_due_ms(const struct ecc_nor *nor, uint32_t now_ms) {
  uint32_t next = UINT32_MAX;

  for (size_t i = 0U; i < ECC_NOR_SLOTS; i++) {
    const struct ecc_nor_slot *slot = &nor->slots[i];

    if (slot->page == ECC_NOR_NO_PAGE) {
      continue;
    }

    for (uint32_t w = 0U; w < ECC_NOR_PAGE_WORDS; w++) {
      uint32_t age;
      uint32_t hold;

      if (!prv_test(slot->pending, w)) {
        continue;
      }

      age = now_ms - slot->since_ms[w];
      hold = prv_hold_ms(slot, w);
      if (age >= hold) {
        return 0U;
      }
      if ((hold - age) < next) {
        next = hold - age;
      }
    }
  }

  return next;
}

int ecc_nor_set_write_through(struct ecc_nor *nor, uint32_t now_ms) {
  nor->write_through = true;

  return ecc_nor_flush(nor, now_ms, true);
}
