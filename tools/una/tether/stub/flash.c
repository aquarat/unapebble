/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The embedded flash, for the PebbleOS slot only: from 0x08080000 to the end
 * of a 4 MiB part, two banks of 256 pages of 8 KiB, programmed by quad-words.
 * Nothing below the slot is ever erased or written; the option bytes are only
 * read (SWAP_BANK, to find the bank of an address).
 */

#include "regs.h"
#include "stub.h"

#define FLASH_MEM     0x08000000U
#define BANK_SIZE     0x200000U
#define OP_TIMEOUT_MS 100U

uint32_t flash_size_kb(void) {
  return *(volatile const uint16_t *)STM32U5_FLASHSIZE_ADDR;
}

/* The slot is laid out for the 4 MiB STM32U5A5 */
bool flash_supported(void) {
  return flash_size_kb() == 4096U;
}

void icache_invalidate(void) {
  uintptr_t cr = ICACHE_BASE + STM32U5_ICACHE_CR_OFFSET;

  if ((rd(cr) & ICACHE_CR_EN) != 0U) {
    setb(cr, ICACHE_CR_CACHEINV);
    while ((rd(ICACHE_BASE + STM32U5_ICACHE_SR_OFFSET) & ICACHE_SR_BUSYF) != 0U) {
    }
  }
}

static bool prv_in_slot(uint32_t addr, uint32_t len) {
  const uint32_t end = FLASH_MEM + flash_size_kb() * 1024U;

  return addr >= TETHER_SLOT_BASE && addr <= end && len <= end - addr;
}

static int prv_wait(void) {
  uint32_t start = millis();

  while ((rd(FLASHR(STM32U5_FLASH_NSSR_OFFSET)) & (FLASH_NSSR_BSY | FLASH_NSSR_WDW)) != 0U) {
    if (millis() - start > OP_TIMEOUT_MS) {
      return TETHER_ERR_TIMEOUT;
    }
  }
  uint32_t sr = rd(FLASHR(STM32U5_FLASH_NSSR_OFFSET));
  wr(FLASHR(STM32U5_FLASH_NSSR_OFFSET), sr & (FLASH_NSSR_ERRORS | FLASH_NSSR_EOP));
  return (sr & FLASH_NSSR_ERRORS) ? TETHER_ERR_FLASH : TETHER_OK;
}

static int prv_unlock(void) {
  uintptr_t cr = FLASHR(STM32U5_FLASH_NSCR_OFFSET);

  if (prv_wait() == TETHER_ERR_TIMEOUT) {
    return TETHER_ERR_TIMEOUT;
  }
  if (rd(cr) & FLASH_NSCR_LOCK) {
    wr(FLASHR(STM32U5_FLASH_NSKEYR_OFFSET), FLASH_NSKEYR_KEY1);
    wr(FLASHR(STM32U5_FLASH_NSKEYR_OFFSET), FLASH_NSKEYR_KEY2);
  }
  return (rd(cr) & FLASH_NSCR_LOCK) ? TETHER_ERR_FLASH : TETHER_OK;
}

static void prv_lock(void) {
  wr(FLASHR(STM32U5_FLASH_NSCR_OFFSET), FLASH_NSCR_LOCK);
  icache_invalidate();
}

int flash_erase(uint32_t addr, uint32_t len) {
  if (!flash_supported()) {
    return TETHER_ERR_UNSUPPORTED;
  }
  if (!prv_in_slot(addr, len) || (addr % TETHER_FLASH_PAGE) != 0U ||
      (len % TETHER_FLASH_PAGE) != 0U) {
    return TETHER_ERR_PROTECTED;
  }
  int ret = prv_unlock();
  const bool swap = (rd(FLASHR(STM32U5_FLASH_OPTR_OFFSET)) & FLASH_OPTR_SWAP_BANK) != 0U;

  for (uint32_t a = addr; ret == TETHER_OK && a < addr + len; a += TETHER_FLASH_PAGE) {
    const uint32_t off = a - FLASH_MEM;
    const bool bank2 = ((off / BANK_SIZE) != 0U) != swap;
    const uint32_t page = (off % BANK_SIZE) / TETHER_FLASH_PAGE;

    /* Never below the slot, whatever the arithmetic above did */
    if (a < TETHER_SLOT_BASE) {
      ret = TETHER_ERR_PROTECTED;
      break;
    }
    wr(FLASHR(STM32U5_FLASH_NSCR_OFFSET),
       FLASH_NSCR_PER | PBL_FIELD_PREP(FLASH_NSCR_PNB_MASK, page) | (bank2 ? FLASH_NSCR_BKER : 0U));
    setb(FLASHR(STM32U5_FLASH_NSCR_OFFSET), FLASH_NSCR_STRT);
    ret = prv_wait();
    watchdog_feed();
  }
  prv_lock();
  return ret;
}

int flash_program(uint32_t addr, const uint8_t *data, uint32_t len) {
  if (!flash_supported()) {
    return TETHER_ERR_UNSUPPORTED;
  }
  if (!prv_in_slot(addr, len) || (addr % TETHER_FLASH_PROG_ALIGN) != 0U ||
      (len % TETHER_FLASH_PROG_ALIGN) != 0U) {
    return TETHER_ERR_PROTECTED;
  }
  int ret = prv_unlock();

  if (ret == TETHER_OK) {
    wr(FLASHR(STM32U5_FLASH_NSCR_OFFSET), FLASH_NSCR_PG);
  }
  for (uint32_t i = 0U; ret == TETHER_OK && i < len; i += 16U) {
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)(addr + i);
    for (unsigned w = 0U; w < 4U; w++) {
      const uint8_t *s = &data[i + 4U * w];
      dst[w] =
          (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
    }
    ret = prv_wait();
  }
  prv_lock();

  for (uint32_t i = 0U; ret == TETHER_OK && i < len; i++) {
    if (*(volatile const uint8_t *)(uintptr_t)(addr + i) != data[i]) {
      ret = TETHER_ERR_FLASH;
    }
  }
  return ret;
}
