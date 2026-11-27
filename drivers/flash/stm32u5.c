/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "ecc_nor.h"
#include "flash_priv.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/flash/stm32u5.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/mmio.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/soc/st/icache.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

#include <hardware/stm32u5_flash.h>

PBL_LOG_MODULE_DECLARE(driver_flash, CONFIG_DRIVER_FLASH_LOG_LEVEL);

#define REG(u5, reg) ((u5)->base + STM32U5_FLASH_##reg##_OFFSET)

// The CPU executes from the first bank, so only the second one is changed
#define BANK_SIZE (CONFIG_FLASH_SIZE / 2U)

// Page erase takes up to 3.4 ms and a quad-word 118 us (DS13086): far more is a fault
#define OP_TIMEOUT_US 100000U

static_assert(STM32U5_FLASH_PAGE_SIZE == ECC_NOR_PAGE_SIZE, "page sizes differ");

struct ecc_nor pbl_flash_stm32u5_nor;

static const struct pbl_flash_stm32u5 *prv_u5(const struct pbl_flash *flash) {
  return PBL_CONTAINER_OF(flash, const struct pbl_flash_stm32u5, flash);
}

static uintptr_t prv_mem(uint32_t addr) {
  return (uintptr_t)CONFIG_FLASH_BASE + addr;
}

static int prv_wait(const struct pbl_flash_stm32u5 *u5) {
  uint32_t sr;
  int ret;

  ret = PBL_POLL_BUSY_US((pbl_readl(REG(u5, NSSR)) & (FLASH_NSSR_BSY | FLASH_NSSR_WDW)) == 0U, 0U,
                         OP_TIMEOUT_US);
  sr = pbl_readl(REG(u5, NSSR));
  pbl_writel(sr & (FLASH_NSSR_ERRORS | FLASH_NSSR_EOP), REG(u5, NSSR));

  if (ret != 0) {
    return ret;
  }

  return ((sr & FLASH_NSSR_ERRORS) != 0U) ? -EIO : 0;
}

static int prv_unlock(const struct pbl_flash_stm32u5 *u5) {
  int ret;

  ret = prv_wait(u5);
  if (ret != 0) {
    return ret;
  }

  if ((pbl_readl(REG(u5, NSCR)) & FLASH_NSCR_LOCK) != 0U) {
    pbl_writel(FLASH_NSKEYR_KEY1, REG(u5, NSKEYR));
    pbl_writel(FLASH_NSKEYR_KEY2, REG(u5, NSKEYR));
  }

  return ((pbl_readl(REG(u5, NSCR)) & FLASH_NSCR_LOCK) != 0U) ? -EPERM : 0;
}

static void prv_lock(const struct pbl_flash_stm32u5 *u5) {
  pbl_writel(FLASH_NSCR_LOCK, REG(u5, NSCR));
}

static bool prv_writable(uint32_t addr, size_t len) {
  return addr >= BANK_SIZE && len <= (CONFIG_FLASH_SIZE - addr);
}

static int prv_hw_program(void *ctx, uint32_t addr, const uint8_t data[ECC_NOR_WORD_SIZE]) {
  const struct pbl_flash_stm32u5 *u5 = ctx;
  volatile uint32_t *dst = (volatile uint32_t *)prv_mem(addr);
  uint32_t words[ECC_NOR_WORD_SIZE / 4U];
  int ret;

  if (!prv_writable(addr, ECC_NOR_WORD_SIZE) || (addr % ECC_NOR_WORD_SIZE) != 0U) {
    return -EACCES;
  }

  memcpy(words, data, sizeof(words));

  ret = prv_unlock(u5);
  if (ret == 0) {
    pbl_writel(FLASH_NSCR_PG, REG(u5, NSCR));

    // The four words of the quad-word, back to back
    pbl_irq_lock();
    for (size_t i = 0U; i < PBL_ARRAY_SIZE(words); i++) {
      dst[i] = words[i];
    }
    pbl_irq_unlock();

    ret = prv_wait(u5);
    pbl_writel(0U, REG(u5, NSCR));
  }
  prv_lock(u5);

  if (ret != 0) {
    PBL_LOG_ERR("Program of 0x%" PRIx32 " failed: %d", addr, ret);
  }

  return ret;
}

static int prv_hw_erase_page(void *ctx, uint32_t addr) {
  const struct pbl_flash_stm32u5 *u5 = ctx;
  const uint32_t page = (addr - BANK_SIZE) / STM32U5_FLASH_PAGE_SIZE;
  uint32_t cr;
  int ret;

  if (!prv_writable(addr, STM32U5_FLASH_PAGE_SIZE) || (addr % STM32U5_FLASH_PAGE_SIZE) != 0U) {
    return -EACCES;
  }

  ret = prv_unlock(u5);
  if (ret == 0) {
    cr = FLASH_NSCR_PER | FLASH_NSCR_BKER | PBL_FIELD_PREP(FLASH_NSCR_PNB_MASK, page);
    pbl_writel(cr, REG(u5, NSCR));
    pbl_writel(cr | FLASH_NSCR_STRT, REG(u5, NSCR));
    ret = prv_wait(u5);
    pbl_writel(0U, REG(u5, NSCR));
  }
  prv_lock(u5);

  if (ret != 0) {
    PBL_LOG_ERR("Erase of 0x%" PRIx32 " failed: %d", addr, ret);
  }

  return ret;
}

static void prv_hw_read(void *ctx, uint32_t addr, void *buf, size_t len) {
  memcpy(buf, (const void *)prv_mem(addr), len);
}

static void prv_hw_sync(void *ctx) {
  soc_stm32u5_icache_invalidate();
}

static const struct ecc_nor_hw s_hw = {
  .program = prv_hw_program,
  .erase_page = prv_hw_erase_page,
  .read = prv_hw_read,
  .sync = prv_hw_sync,
};

static uint32_t prv_now_ms(void) {
  return (uint32_t)pbl_uptime_ms();
}

static void prv_flush_fn(struct pbl_work *work) {
  struct pbl_flash_stm32u5_state *st =
      PBL_CONTAINER_OF(work, struct pbl_flash_stm32u5_state, flush.work);
  const struct pbl_flash *flash = st->flash;
  uint32_t next;

  if (pbl_flash__access_begin(flash, 0U) != 0) {
    return;
  }

  (void)ecc_nor_flush(prv_u5(flash)->nor, prv_now_ms(), false);
  next = ecc_nor_next_due_ms(prv_u5(flash)->nor, prv_now_ms());
  pbl_flash__access_end(flash);

  if (next != UINT32_MAX) {
    (void)pbl_delayed_work_schedule(&pbl_sys_workq, &st->flush, PBL_MSEC(next));
  }
}

static int prv_init(const struct pbl_flash *flash) {
  const struct pbl_flash_stm32u5 *u5 = prv_u5(flash);
  const uint32_t size_kib = *(const volatile uint16_t *)STM32U5_FLASHSIZE_ADDR;

  if (pbl_flash_in_coredump(flash) && u5->nor->hw != nullptr) {
    return ecc_nor_set_write_through(u5->nor, prv_now_ms());
  }

  // Partitions and the bank written are the 4 MiB part's
  if ((size_kib * 1024U) != CONFIG_FLASH_SIZE) {
    PBL_LOG_ERR("Flash of %" PRIu32 " KiB, not %u", size_kib, CONFIG_FLASH_SIZE / 1024U);
    return -ENODEV;
  }

  // Swapped banks would put the code in the bank written
  if ((pbl_readl(REG(u5, OPTR)) & FLASH_OPTR_SWAP_BANK) != 0U) {
    PBL_LOG_ERR("Flash banks swapped");
    return -ENODEV;
  }

  u5->state->flash = flash;
  ecc_nor_init(u5->nor, &s_hw, (void *)u5);
  if (pbl_flash_in_coredump(flash)) {
    return ecc_nor_set_write_through(u5->nor, prv_now_ms());
  }

  pbl_delayed_work_init(&u5->state->flush, prv_flush_fn);

  return 0;
}

static int prv_read(const struct pbl_flash *flash, uint32_t addr, void *buf, size_t len) {
  ecc_nor_read(prv_u5(flash)->nor, addr, buf, len);

  return 0;
}

static int prv_write(const struct pbl_flash *flash, uint32_t addr, const void *buf, size_t len) {
  const struct pbl_flash_stm32u5 *u5 = prv_u5(flash);
  uint32_t next;
  int ret;

  if (!prv_writable(addr, len)) {
    return -EACCES;
  }

  ret = ecc_nor_write(u5->nor, addr, buf, len, prv_now_ms());
  if (ret != 0 || pbl_flash_in_coredump(flash) ||
      pbl_delayed_work_is_pending(&u5->state->flush)) {
    return ret;
  }

  next = ecc_nor_next_due_ms(u5->nor, prv_now_ms());
  if (next != UINT32_MAX) {
    (void)pbl_delayed_work_schedule(&pbl_sys_workq, &u5->state->flush, PBL_MSEC(next));
  }

  return 0;
}

static int prv_erase(const struct pbl_flash *flash, uint32_t addr, uint32_t size) {
  int ret;

  if (!prv_writable(addr, size)) {
    return -EACCES;
  }

  ret = ecc_nor_erase(prv_u5(flash)->nor, addr, size, prv_now_ms());

  return (ret == 0) ? 1 : ret;
}

static void prv_stop(const struct pbl_flash *flash) {
  (void)ecc_nor_flush(prv_u5(flash)->nor, prv_now_ms(), true);
}

const struct pbl_flash_ops pbl_flash_stm32u5_ops = {
  .init = prv_init,
  .read = prv_read,
  .write = prv_write,
  .erase = prv_erase,
  .stop = prv_stop,
};

#include PBL_DT_INSTANCES(st_stm32u5_flash_controller)
