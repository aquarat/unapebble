/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The report of the run before: how it ended (the SoC's reset flags, PebbleOS's reboot reason),
// where it was (its breadcrumbs) and what failed (its fault and panic records).

#include "lab.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <pbl/boot_trace/boot_trace.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#include <system/bootbits.h>
#include <system/reboot_reason.h>

static const char *const s_kernel_steps[] = {
  "", "start", "devices", "main()", "boot done", "reboot", "power off", "emergency reset",
};

static const char *const s_main_steps[] = {
  "",           "start",         "clock restore",
  "splash",     "drivers",       "clock init",
  "flash log",  "debug init",    "early services (PFS mount)",
  "prf update", "resource init", "system resources",
  "compositor", "bluetooth",     "services",
  "launcher",
};

static const char *const s_sdmmc_steps[] = {
  "",        "supply on",   "clock 400k",  "CMD0",        "CMD1 (OCR)",   "CID/RCA/select",
  "EXT_CSD", "partition",   "power class", "bus width 4", "speed switch", "EXT_CSD again",
  "ready",   "init failed",
};

static const char *const s_nor_steps[] = {"", "init", "ready", "init failed"};

static const char *const s_pfs_steps[] = {
  "",           "init",      "check active",  "format: erase", "format: headers", "format done",
  "GC recover", "GC region", "prepare space", "ready",
};

static const char *const s_batt_steps[] = {
  "", "charger init", "charger ready", "gauge init", "gauge ready"
};

static const char *prv_name(const char *const *names, size_t n, uint32_t step) {
  return (step < n && names[step][0] != '\0') ? names[step] : nullptr;
}

void lab_crumb_name(uint32_t crumb, char *buf, size_t size) {
  const uint8_t area = PBL_BOOT_TRACE_AREA(crumb);
  const uint32_t step = PBL_BOOT_TRACE_STEP(crumb);
  const char *name = nullptr;

  switch (area) {
    case PBL_BT_NONE:
      snprintf(buf, size, "none");
      return;
    case PBL_BT_KERNEL:
      name = prv_name(s_kernel_steps, PBL_ARRAY_SIZE(s_kernel_steps), step);
      break;
    case PBL_BT_MAIN:
      name = prv_name(s_main_steps, PBL_ARRAY_SIZE(s_main_steps), step);
      break;
    case PBL_BT_SDMMC:
      name = prv_name(s_sdmmc_steps, PBL_ARRAY_SIZE(s_sdmmc_steps), step & 0xFFU);
      if (name != nullptr && (step & 0xFF00U) != 0U) {
        snprintf(buf, size, "sdmmc %s (HS)", name);
        return;
      }
      break;
    case PBL_BT_NOR:
      if ((step & PBL_BT_NOR_ERASE) != 0U) {
        snprintf(buf, size, "nor erase at %lu KiB", (unsigned long)(step & 0xFFFFU) * 4UL);
        return;
      }
      name = prv_name(s_nor_steps, PBL_ARRAY_SIZE(s_nor_steps), step);
      break;
    case PBL_BT_PFS:
      name = prv_name(s_pfs_steps, PBL_ARRAY_SIZE(s_pfs_steps), step);
      break;
    case PBL_BT_BATT:
      name = prv_name(s_batt_steps, PBL_ARRAY_SIZE(s_batt_steps), step);
      break;
    case PBL_BT_MMC: {
      static const char *const s_mmc[] = {
        "",        "power-up try", "card on", "power-up failed", "read failed", "write failed",
        "card off"
      };
      const uint32_t kind = step >> 8;

      if (kind < PBL_ARRAY_SIZE(s_mmc) && kind != 0U) {
        snprintf(buf, size, "mmc %s %lu", s_mmc[kind], (unsigned long)(step & 0xFFU));
        return;
      }
      break;
    }
    case PBL_BT_RES:
      if ((step & PBL_BT_RES_COPY) != 0U) {
        snprintf(buf, size, "res copy at %lu KiB", (unsigned long)(step & 0xFFFFU) * 4UL);
        return;
      }
      switch (step & 0xF0U) {
        case PBL_BT_RES_CHECK_BANK:
          snprintf(buf, size, "res check bank %lu", (unsigned long)(step & 0xFU));
          return;
        case PBL_BT_RES_FOUND:
          snprintf(buf, size, "res found in bank %lu", (unsigned long)(step & 0xFU));
          return;
        case PBL_BT_RES_INSTALL:
          snprintf(buf, size, "res install to bank %lu", (unsigned long)(step & 0xFU));
          return;
        default:
          break;
      }
      switch (step) {
        case PBL_BT_RES_MOUNT:
          name = "update drive mount";
          break;
        case PBL_BT_RES_SEARCH:
          name = "update drive search";
          break;
        case PBL_BT_RES_ERASE:
          name = "bank erase";
          break;
        case PBL_BT_RES_INSTALLED:
          name = "installed";
          break;
        case PBL_BT_RES_INSTALL_FAILED:
          name = "install failed";
          break;
        case PBL_BT_RES_MISSING:
          name = "MISSING, power off";
          break;
        default:
          break;
      }
      break;
    default:
      if (area >= PBL_BT_LAB && (size_t)(area - PBL_BT_LAB) < lab_num_experiments) {
        const struct lab_experiment *exp = lab_experiments[area - PBL_BT_LAB];

        if (step == 0xFFFFU) {
          snprintf(buf, size, "Lab %s: done", exp->title);
        } else if (step == 0xFFF0U) {
          snprintf(buf, size, "Lab opening");
        } else if (step >= 1U && step <= exp->num_steps) {
          snprintf(buf, size, "Lab %s/%lu %s", exp->title, (unsigned long)step,
                   exp->steps[step - 1U].name);
        } else {
          snprintf(buf, size, "Lab %s/%lu", exp->title, (unsigned long)step);
        }
        return;
      }
      break;
  }

  if (name != nullptr) {
    snprintf(buf, size, "%s %s", pbl_boot_trace_area_name(area), name);
  } else {
    snprintf(buf, size, "%s/0x%lx", pbl_boot_trace_area_name(area), (unsigned long)step);
  }
}

static void prv_reset_flags(uint32_t csr, char *buf, size_t size) {
  static const struct {
    uint8_t bit;
    const char *name;
  } s_flags[] = {
    {31, "low-power"},    {30, "WWDG"}, {29, "IWDG"},         {28, "software"},
    {27, "BOR/power-on"}, {26, "pin"},  {25, "option bytes"},
  };
  size_t n = 0U;

  buf[0] = '\0';
  for (size_t i = 0U; i < PBL_ARRAY_SIZE(s_flags); i++) {
    if ((csr & (1UL << s_flags[i].bit)) != 0U && n < size) {
      n += (size_t)snprintf(&buf[n], size - n, "%s%s", n == 0U ? "" : " ", s_flags[i].name);
    }
  }
  if (n == 0U) {
    snprintf(buf, size, "no flag");
  }
}

static const char *prv_reason_name(uint8_t code) {
  switch (code) {
    case RebootReasonCode_Unknown:
      return "none";
    case RebootReasonCode_LowBattery:
      return "low battery";
    case RebootReasonCode_SoftwareUpdate:
      return "software update";
    case RebootReasonCode_ResetButtonsHeld:
      return "reset buttons";
    case RebootReasonCode_ShutdownMenuItem:
      return "shutdown";
    case RebootReasonCode_PrfReset:
      return "PRF reset";
    case RebootReasonCode_Watchdog:
      return "watchdog";
    case RebootReasonCode_Assert:
      return "assert";
    case RebootReasonCode_StackOverflow:
      return "stack overflow";
    case RebootReasonCode_HardFault:
      return "hard fault";
    case RebootReasonCode_LauncherPanic:
      return "launcher panic";
    case RebootReasonCode_EventQueueFull:
      return "event queue full";
    case RebootReasonCode_OutOfMemory:
      return "out of memory";
    case RebootReasonCode_CoreDump:
      return "core dump";
    case RebootReasonCode_CoreDumpEntryFailed:
      return "core dump failed";
    default:
      return "other";
  }
}

static const char *prv_fault_name(uint32_t kind) {
  static const char *const s_kinds[] = {"",         "stack overflow", "MemManage",
                                        "BusFault", "UsageFault",     "HardFault"};

  return kind < PBL_ARRAY_SIZE(s_kinds) ? s_kinds[kind] : "fault";
}

static const char *prv_image_name(uint32_t id) {
  switch (id) {
    case 1:
      return "Lab";
    case 2:
      return "eMMC (B) traced";
    case 3:
      return "eMMC fixed, traced";
    default:
      return "other";
  }
}

static void prv_ms(char *buf, size_t size, uint32_t ms) {
  snprintf(buf, size, "%lu.%02lus", (unsigned long)(ms / 1000U), (unsigned long)(ms % 1000U) / 10U);
}

// The last breadcrumb not of the kernel's reboot path: where the run was
static uint32_t prv_last_place(const struct pbl_boot_trace_record *rec, uint32_t *ms) {
  const uint32_t n = PBL_MIN(rec->trail_count, PBL_BOOT_TRACE_TRAIL);

  for (uint32_t k = 0U; k < n; k++) {
    const uint32_t i = (rec->trail_count - 1U - k) % PBL_BOOT_TRACE_TRAIL;
    const uint32_t crumb = rec->trail[i].crumb;
    const uint32_t step = PBL_BOOT_TRACE_STEP(crumb);

    if (PBL_BOOT_TRACE_AREA(crumb) == PBL_BT_KERNEL &&
        (step == PBL_BT_KERNEL_REBOOT || step == PBL_BT_KERNEL_POWER_OFF ||
         step == PBL_BT_KERNEL_EMERGENCY)) {
      continue;
    }
    *ms = rec->trail[i].uptime_ms;
    return crumb;
  }

  *ms = 0U;
  return 0U;
}

uint32_t lab_previous_place(uint32_t *ms) {
  const struct pbl_boot_trace_boot *boot = pbl_boot_trace_boot_info();
  uint32_t t = 0U;
  uint32_t crumb = 0U;

  if (boot->ram_valid) {
    crumb = prv_last_place(&boot->previous, &t);
  } else if (boot->bkp_valid) {
    crumb = boot->bkp_crumb;
  }
  if (ms != nullptr) {
    *ms = t;
  }

  return crumb;
}

const char *lab_boot_summary(void) {
  static char s_summary[112];
  const struct pbl_boot_trace_boot *boot = pbl_boot_trace_boot_info();
  char flags[48];
  char place[48] = "";

  prv_reset_flags(boot->reset_flags, flags, sizeof(flags));
  if (boot->ram_valid || boot->bkp_valid) {
    lab_crumb_name(lab_previous_place(nullptr), place, sizeof(place));
  }
  snprintf(s_summary, sizeof(s_summary), "%.47s; %.47s", flags, place);

  return s_summary;
}

static void prv_report_record(const struct pbl_boot_trace_record *rec) {
  char name[64];
  char t[16];
  uint32_t ms;
  uint32_t place;
  const RebootReason *rr = (const RebootReason *)rec->reboot_reason;

  lab_printf("-- Run before --");
  lab_printf("%.8s #%lu", rec->image, (unsigned long)rec->boot_count);
  lab_printf("%.32s", rec->version);

  place = prv_last_place(rec, &ms);
  lab_crumb_name(place, name, sizeof(name));
  prv_ms(t, sizeof(t), ms);
  lab_printf("Stopped at:");
  lab_printf("%s", name);
  lab_printf("at %s", t);

  if (rec->fault.kind != 0U) {
    const struct pbl_boot_trace_fault *f = &rec->fault;

    prv_ms(t, sizeof(t), f->uptime_ms);
    lab_printf("-- %s at %s --", prv_fault_name(f->kind), t);
    lab_printf("PC 0x%08" PRIx32, f->pc);
    lab_printf("LR 0x%08" PRIx32, f->lr);
    lab_printf("thread %.12s", f->thread);
    lab_printf("CFSR 0x%08" PRIx32, f->cfsr);
    lab_printf("HFSR 0x%08" PRIx32, f->hfsr);
    lab_printf("BFAR 0x%08" PRIx32, f->bfar);
    lab_printf("MMFAR 0x%08" PRIx32, f->mmfar);
    lab_printf("PSR 0x%08" PRIx32 " SP 0x%08" PRIx32, f->psr, f->sp);
    lab_printf("EXC_RET 0x%08" PRIx32, f->exc_return);
    lab_crumb_name(f->crumb, name, sizeof(name));
    lab_printf("during %s", name);
  }

  if (rec->panic.valid != 0U) {
    const struct pbl_boot_trace_panic *p = &rec->panic;

    prv_ms(t, sizeof(t), p->uptime_ms);
    lab_printf("-- Panic at %s --", t);
    lab_printf("%.24s:%lu", p->file, (unsigned long)p->line);
    lab_printf("LR 0x%08" PRIx32, p->lr);
    lab_printf("%.64s", p->msg);
    lab_crumb_name(p->crumb, name, sizeof(name));
    lab_printf("during %s", name);
  }

  if (rec->notes_set != 0U) {
    static const char *const s_notes[] = {"res crc wanted", "mmc error",  "card blocks", "card OCR",
                                          "sdmmc CMD/STA",  "res copied", "value"};

    lab_printf("-- Notes --");
    for (size_t i = 0U; i < PBL_ARRAY_SIZE(s_notes); i++) {
      if ((rec->notes_set & (1U << i)) != 0U) {
        lab_printf("%s 0x%08" PRIx32, s_notes[i], rec->notes[i]);
      }
    }
  }

  lab_printf("-- Last steps --");
  const uint32_t n = PBL_MIN(rec->trail_count, PBL_BOOT_TRACE_TRAIL);
  for (uint32_t k = (n > 16U) ? n - 16U : 0U; k < n; k++) {
    const uint32_t i = (rec->trail_count - n + k) % PBL_BOOT_TRACE_TRAIL;

    lab_crumb_name(rec->trail[i].crumb, name, sizeof(name));
    prv_ms(t, sizeof(t), rec->trail[i].uptime_ms);
    lab_printf("%s %s", t, name);
  }

  // What the run before was started with: how the one before it ended
  char flags[48];
  prv_reset_flags(rec->reset_flags, flags, sizeof(flags));
  lab_printf("-- It started after --");
  lab_printf("%s", flags);
  lab_printf("reason %s", prv_reason_name(rr->code));
}

static int prv_report(void) {
  const struct pbl_boot_trace_boot *boot = pbl_boot_trace_boot_info();
  const RebootReason *rr;
  char flags[48];
  char name[64];

  // The reboot reason the run before set, as this boot found it before clearing it
  rr = (const RebootReason *)pbl_boot_trace_current()->reboot_reason;

  prv_reset_flags(boot->reset_flags, flags, sizeof(flags));
  lab_printf("Reset: %s", flags);
  lab_printf("RCC CSR 0x%08" PRIx32, boot->reset_flags);
  lab_printf("Reason: %s%s", prv_reason_name(rr->code), rr->restarted_safely ? ", safely" : "");
  if (rr->code != RebootReasonCode_Unknown) {
    lab_printf("value 0x%08" PRIx32, rr->extra.value);
    if (rr->code == RebootReasonCode_Watchdog) {
      lab_printf("stuck PC 0x%08" PRIx32, rr->watchdog.stuck_task_pc);
      lab_printf("stuck LR 0x%08" PRIx32, rr->watchdog.stuck_task_lr);
      lab_printf("work 0x%08" PRIx32, rr->watchdog.stuck_task_callback);
    }
  }

  if (boot->bkp_valid) {
    lab_crumb_name(boot->bkp_crumb, name, sizeof(name));
    lab_printf("Last crumb (TAMP):");
    lab_printf("%s", name);
    lab_printf("by %s, boot #%lu", prv_image_name((boot->bkp_boot >> 20) & 0xFU),
               (unsigned long)(boot->bkp_boot & 0xFFFFFU));
  } else {
    lab_printf("No breadcrumb kept");
  }

  if (boot->ram_valid) {
    lab_printf("RAM record: %s", boot->ram_current ? "kept" : "of an older run");
    prv_report_record(&boot->previous);
  } else {
    lab_printf("RAM record: none (power was lost, or cleared)");
  }

  lab_printf("-- This boot --");
  lab_printf("Lab #%lu", (unsigned long)pbl_boot_trace_current()->boot_count);
  lab_printf("boot bits 0x%08" PRIx32, boot_bits_get());
  lab_printf("PCs: addr2line with the ELF of the image that ran");
  lab_printf("Back: experiments");

  return 0;
}

void lab_boot_report(void) {
  (void)prv_report();
}

static const struct lab_step s_steps[] = {
  {"report", prv_report},
};

const struct lab_experiment lab_exp_boot = {
  .title = "Last boot",
  .subtitle = "Reset cause, where it stopped",
  .steps = s_steps,
  .num_steps = PBL_ARRAY_SIZE(s_steps),
};
