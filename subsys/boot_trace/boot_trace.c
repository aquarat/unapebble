/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <pbl/boot_trace/boot_trace.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/mmio.h>
#include <pbl/kernel/sched.h>
#include <pbl/kernel/time.h>
#include <pbl/system/rtc_registers.h>
#include <pbl/util/size.h>
#include <pbl/version.h>

#include <devicetree/regs.h>
#include <hardware/stm32u5_rcc.h>

#define RECORD_MAGIC     0x42545231U /* BTR1 */
#define RECORD_MAGIC_END 0x31525442U
#define BKP_MAGIC        0xB7U

static_assert(sizeof(struct pbl_boot_trace_record) <= CONFIG_BOOT_TRACE_RAM_SIZE,
              "boot trace record larger than its RAM");
static_assert(CONFIG_BOOT_TRACE_BKP_CRUMB >= 24 && CONFIG_BOOT_TRACE_BKP_BOOT >= 24,
              "backup registers 0 to 23 are the stock firmware's");

#define RECORD ((struct pbl_boot_trace_record *)CONFIG_BOOT_TRACE_RAM_BASE)

static struct pbl_boot_trace_boot s_boot;

static const char *const s_area_names[] = {
  [PBL_BT_NONE] = "none", [PBL_BT_KERNEL] = "kernel", [PBL_BT_MAIN] = "main",
  [PBL_BT_MMC] = "mmc",   [PBL_BT_SDMMC] = "sdmmc",   [PBL_BT_NOR] = "nor",
  [PBL_BT_PFS] = "pfs",   [PBL_BT_RES] = "res",       [PBL_BT_BATT] = "batt",
};

const char *pbl_boot_trace_area_name(uint8_t area) {
  if (area >= PBL_BT_LAB) {
    return "lab";
  }
  if (area < PBL_ARRAY_SIZE(s_area_names) && s_area_names[area] != nullptr) {
    return s_area_names[area];
  }

  return "?";
}

static uint32_t prv_uptime_ms(void) {
  return pbl_kernel_is_running() ? (uint32_t)pbl_uptime_ms() : 0U;
}

static void prv_copy_str(char *dst, size_t size, const char *src) {
  size_t i = 0U;

  if (src != nullptr) {
    for (; i + 1U < size && src[i] != '\0'; i++) {
      dst[i] = src[i];
    }
  }
  dst[i] = '\0';
}

static bool prv_record_valid(const struct pbl_boot_trace_record *rec) {
  return rec->magic == RECORD_MAGIC && rec->magic_end == RECORD_MAGIC_END;
}

void pbl_boot_trace_init(void) {
  struct pbl_boot_trace_record *rec = RECORD;
  uint32_t count;

  s_boot.reset_flags = pbl_readl(PBL_DT_REG_RCC + STM32U5_RCC_CSR_OFFSET);
  s_boot.bkp_crumb = retained_read(CONFIG_BOOT_TRACE_BKP_CRUMB);
  s_boot.bkp_boot = retained_read(CONFIG_BOOT_TRACE_BKP_BOOT);
  s_boot.bkp_valid = (s_boot.bkp_boot >> 24) == BKP_MAGIC;
  s_boot.ram_valid = prv_record_valid(rec);
  if (s_boot.ram_valid) {
    s_boot.previous = *rec;
    s_boot.ram_current =
        s_boot.bkp_valid && (rec->boot_count & 0xFFFFFU) == (s_boot.bkp_boot & 0xFFFFFU);
  }

  count = s_boot.bkp_valid ? ((s_boot.bkp_boot + 1U) & 0xFFFFFU) : 1U;

  memset(rec, 0, sizeof(*rec));
  rec->boot_count = count;
  prv_copy_str(rec->image, sizeof(rec->image), CONFIG_BOOT_TRACE_IMAGE);
  prv_copy_str(rec->version, sizeof(rec->version), PBL_VERSION_TAG);
  rec->reset_flags = s_boot.reset_flags;
  rec->reboot_reason[0] = retained_read(REBOOT_REASON_REGISTER_1);
  rec->reboot_reason[1] = retained_read(REBOOT_REASON_STUCK_TASK_PC);
  rec->reboot_reason[2] = retained_read(REBOOT_REASON_STUCK_TASK_LR);
  rec->reboot_reason[3] = retained_read(REBOOT_REASON_STUCK_TASK_CALLBACK);
  rec->magic_end = RECORD_MAGIC_END;
  rec->magic = RECORD_MAGIC;

  retained_write(CONFIG_BOOT_TRACE_BKP_BOOT,
                 (BKP_MAGIC << 24) | ((CONFIG_BOOT_TRACE_IMAGE_ID & 0xFU) << 20) | count);
  pbl_boot_trace_crumb(PBL_BT_KERNEL, PBL_BT_KERNEL_START);
}

void pbl_boot_trace_crumb(uint8_t area, uint32_t step) {
  struct pbl_boot_trace_record *rec = RECORD;
  const uint32_t crumb = PBL_BOOT_TRACE_CRUMB(area, step);
  const uint32_t ms = prv_uptime_ms();

  retained_write(CONFIG_BOOT_TRACE_BKP_CRUMB, crumb);

  pbl_irq_lock();
  const uint32_t i = rec->trail_count % PBL_BOOT_TRACE_TRAIL;
  rec->trail[i].crumb = crumb;
  rec->trail[i].uptime_ms = ms;
  rec->trail_count++;
  pbl_irq_unlock();
}

void pbl_boot_trace_note(enum pbl_boot_trace_note note, uint32_t value) {
  if ((unsigned int)note < PBL_BT_NUM_NOTES) {
    RECORD->notes[note] = value;
    RECORD->notes_set |= 1U << note;
  }
}

void pbl_boot_trace_fault(const struct pbl_boot_trace_fault *fault) {
  struct pbl_boot_trace_record *rec = RECORD;

  // The first fault is the cause; one while handling it is not
  if (rec->fault.kind != 0U) {
    return;
  }
  rec->fault = *fault;
  rec->fault.crumb = retained_read(CONFIG_BOOT_TRACE_BKP_CRUMB);
  rec->fault.uptime_ms = prv_uptime_ms();
}

void pbl_boot_trace_panic(const char *file, int line, uintptr_t lr, const char *msg) {
  struct pbl_boot_trace_panic *p = &RECORD->panic;

  if (p->valid != 0U) {
    return;
  }
  p->lr = lr;
  p->line = (uint32_t)line;
  p->uptime_ms = prv_uptime_ms();
  p->crumb = retained_read(CONFIG_BOOT_TRACE_BKP_CRUMB);
  prv_copy_str(p->file, sizeof(p->file), file);
  prv_copy_str(p->msg, sizeof(p->msg), msg);
  p->valid = 1U;
}

const struct pbl_boot_trace_boot *pbl_boot_trace_boot_info(void) {
  return &s_boot;
}

const struct pbl_boot_trace_record *pbl_boot_trace_current(void) {
  return RECORD;
}
