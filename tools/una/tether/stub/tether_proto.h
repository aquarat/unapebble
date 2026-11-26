/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The tether protocol, shared by the stub, its host-side simulator and, through
 * test_tether.py, which checks these values against tether.py.
 *
 * A frame, both ways: A5 5A, cmd, seq, payload length (u16), payload, then the
 * CRC-32 (zlib's) of cmd to the end of the payload. All values little-endian.
 * A reply has the cmd of its request with bit 7 set and the same seq; its
 * payload starts with a u32 status.
 */

#pragma once

#include <stdint.h>

#define TETHER_MAGIC0 0xa5U
#define TETHER_MAGIC1 0x5aU

#define TETHER_PROTO_VERSION 1U

#define TETHER_HDR_LEN     6U
#define TETHER_CRC_LEN     4U
#define TETHER_MAX_DATA    4096U
#define TETHER_MAX_PAYLOAD (TETHER_MAX_DATA + 64U)
#define TETHER_MAX_FRAME   (TETHER_HDR_LEN + TETHER_MAX_PAYLOAD + TETHER_CRC_LEN)

/* A frame left half received this long is dropped */
#define TETHER_RX_TIMEOUT_MS 500U

#define TETHER_REPLY 0x80U

enum tether_cmd {
  TETHER_CMD_INFO = 0x01,
  TETHER_CMD_READ = 0x02,
  TETHER_CMD_WRITE = 0x03,
  TETHER_CMD_FILL = 0x04,
  TETHER_CMD_CRC32 = 0x05,
  TETHER_CMD_CALL = 0x06,
  TETHER_CMD_I2C = 0x07,
  TETHER_CMD_GPIO = 0x08,
  TETHER_CMD_RESET = 0x09,
  TETHER_CMD_FLASH_ERASE = 0x0a,
  TETHER_CMD_FLASH_WRITE = 0x0b,
  TETHER_CMD_BOOT = 0x0c,
  TETHER_CMD_NOTE = 0x0d,
  TETHER_CMD_CLEAR_FAULT = 0x0e,
  /* Sent for a frame that failed its CRC, whatever its cmd */
  TETHER_CMD_BAD_FRAME = 0x7f,
};

enum tether_status {
  TETHER_OK = 0,
  TETHER_ERR_CRC = 1,
  TETHER_ERR_CMD = 2,
  TETHER_ERR_ARG = 3,
  /* A fault was caught: the payload is a struct tether_fault */
  TETHER_ERR_FAULT = 4,
  TETHER_ERR_TIMEOUT = 5,
  /* Refused: below 0x08080000, the option bytes, the stub's own RAM, the PMIC... */
  TETHER_ERR_PROTECTED = 6,
  TETHER_ERR_NACK = 7,
  TETHER_ERR_BUS = 8,
  TETHER_ERR_FLASH = 9,
  TETHER_ERR_LENGTH = 10,
  TETHER_ERR_UNSUPPORTED = 11,
};

/* CALL flags */
#define TETHER_CALL_BLOB     (1U << 0) /* r0 = api, r1 = args, r2 = nargs; else r0-r3 = args */
#define TETHER_CALL_ANYWHERE (1U << 1) /* allow a target outside the load region */
#define TETHER_CALL_KEEP_OUT (1U << 2) /* append to the output buffer instead of clearing it */
#define TETHER_CALL_MAX_ARGS 12U

/* I2C flags */
#define TETHER_I2C_UNSAFE (1U << 0) /* allow writes to the PMIC, the gauge and the USB-PD chip */
#define TETHER_I2C_400K   (1U << 1)
#define TETHER_I2C_MAX    255U

/* GPIO ops */
enum tether_gpio_op {
  TETHER_GPIO_READ = 0,
  TETHER_GPIO_OUT = 1,    /* push-pull output, arg = level */
  TETHER_GPIO_IN = 2,     /* input, arg = 0 none, 1 pull-up, 2 pull-down */
  TETHER_GPIO_OUT_OD = 3, /* open-drain output, arg = level */
  TETHER_GPIO_ANALOG = 4,
};

enum tether_reset_mode {
  TETHER_RESET_SYSTEM = 0,
  /* Powers off through the bootloader, as PebbleOS does (TAMP BKP3R = 1) */
  TETHER_RESET_POWER_OFF = 1,
};

/* Memory layout */
#define TETHER_STUB_FLASH      0x08060000U
#define TETHER_STUB_FLASH_SIZE 0x20000U
#define TETHER_SLOT_BASE       0x08080000U
#define TETHER_STUB_RAM        0x20000000U
#define TETHER_STUB_RAM_SIZE   0x20000U
#define TETHER_LOAD_BASE       0x20020000U
#define TETHER_LOAD_END        0x2024e000U
#define TETHER_BLOB_STACK_TOP  0x2025e000U
/* The 256 bytes at the top of the stock kernel's retained RAM, which it leaves unused */
#define TETHER_RETAINED 0x2026ff00U

#define TETHER_FLASH_PROG_ALIGN 16U
#define TETHER_FLASH_PAGE       0x2000U

/* A blob image starts with this header, at the address it is linked at */
#define TETHER_BLOB_MAGIC 0x424c4254U /* "TBLB" */

#define TETHER_FAULT_MAGIC 0x544c4146U /* "FALT" */

struct tether_fault {
  uint32_t magic;
  uint32_t exc; /* exception number, IPSR; 0x100 + n: an unexpected IRQ; 0x200 a timeout */
  uint32_t exc_return;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t mmfar;
  uint32_t bfar;
  uint32_t afsr;
  uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
  uint32_t sp;
  uint32_t r4, r5, r6, r7, r8, r9, r10, r11;
  uint32_t uptime_ms;
  uint32_t count;
} __attribute__((packed));

#define TETHER_FAULT_TIMEOUT 0x200U

enum tether_usb_state {
  TETHER_USB_OFF = 0,
  TETHER_USB_WAITING = 1,
  TETHER_USB_RESET = 2,
  TETHER_USB_CONFIGURED = 3,
  TETHER_USB_OPEN = 4,
  TETHER_USB_SUSPENDED = 5,
  TETHER_USB_ERROR = 6,
};

/* INFO flags */
#define TETHER_INFO_PLL           (1U << 0)
#define TETHER_INFO_HSE           (1U << 1)
#define TETHER_INFO_LSE           (1U << 2)
#define TETHER_INFO_DISPLAY       (1U << 3)
#define TETHER_INFO_BOOT_FAULT    (1U << 4)
#define TETHER_INFO_SLOT_VALID    (1U << 5)
#define TETHER_INFO_USB_FS_FORCED (1U << 6)
#define TETHER_INFO_FLASH_OK      (1U << 7)

struct tether_info {
  char magic[4]; /* "UTS1" */
  uint16_t proto_version;
  uint16_t info_size;
  char version[32];
  uint32_t reset_csr; /* RCC_CSR at boot, before its flags were cleared */
  uint32_t sysclk_hz;
  uint32_t hclk_hz;
  uint32_t uid[3];
  uint32_t flash_kb;
  uint32_t idcode; /* DBGMCU_IDCODE */
  uint32_t optr;   /* FLASH_OPTR, read only */
  uint32_t uptime_ms;
  uint8_t usb_speed; /* 0 unknown, 1 full speed, 2 high speed */
  uint8_t usb_state;
  uint8_t buttons; /* bit 0 back, 1 up, 2 select, 3 down: pressed */
  uint8_t usb_error;
  uint32_t flags;
  uint32_t load_base, load_end, blob_stack_top;
  uint32_t out_buf, out_size, out_len;
  uint32_t api;
  uint32_t slot_base, slot_end;
  uint32_t max_data;
  uint32_t cmd_count, err_count;
  uint32_t bkp3r;
  struct tether_fault boot_fault; /* the fault that reset the stub last, if any */
  struct tether_fault last_fault; /* the last fault caught since boot */
} __attribute__((packed));

struct tether_call_result {
  uint32_t r0;
  uint32_t r1;
  uint32_t elapsed_us;
  uint32_t out_len; /* in the buffer; what follows the result is its start */
  uint32_t out_addr;
  uint32_t faulted; /* 1: a fault record follows the output (a fault, or where a timeout hit) */
} __attribute__((packed));
