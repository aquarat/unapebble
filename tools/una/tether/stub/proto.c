/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The tether protocol: frames in, commands checked and run, replies out. It
 * knows nothing of the hardware (port.h), so that the host tests run it as it
 * runs on the watch.
 */

#include "port.h"
#include "tether_proto.h"

#include <string.h>

/* zlib's CRC-32, a nibble at a time */
uint32_t tether_crc32(uint32_t crc, const void *data, size_t len) {
  static const uint32_t s_tab[16] = {
    0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU, 0x76dc4190U, 0x6b6b51f4U,
    0x4db26158U, 0x5005713cU, 0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
    0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU,
  };
  const uint8_t *p = data;

  crc = ~crc;
  while (len-- != 0U) {
    crc ^= *p++;
    crc = (crc >> 4) ^ s_tab[crc & 0xfU];
    crc = (crc >> 4) ^ s_tab[crc & 0xfU];
  }
  return ~crc;
}

static uint8_t s_in[TETHER_MAX_FRAME];
static uint32_t s_pos;
static uint32_t s_last_ms;
static uint8_t s_out[TETHER_MAX_FRAME];

static uint32_t prv_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t prv_u16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static void prv_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* The reply's data, after its status */
#define REPLY_DATA (&s_out[TETHER_HDR_LEN + 4U])
#define REPLY_MAX  (TETHER_MAX_PAYLOAD - 4U)

static void prv_reply(uint8_t cmd, uint8_t seq, int status, uint32_t data_len) {
  const uint32_t len = 4U + data_len;

  s_out[0] = TETHER_MAGIC0;
  s_out[1] = TETHER_MAGIC1;
  s_out[2] = (uint8_t)(cmd | TETHER_REPLY);
  s_out[3] = seq;
  s_out[4] = (uint8_t)len;
  s_out[5] = (uint8_t)(len >> 8);
  prv_put32(&s_out[TETHER_HDR_LEN], (uint32_t)status);
  prv_put32(&s_out[TETHER_HDR_LEN + len], tether_crc32(0U, &s_out[2], 4U + len));
  port_send(s_out, TETHER_HDR_LEN + len + TETHER_CRC_LEN);
}

static bool prv_overlaps(uint32_t addr, uint32_t len, uint32_t start, uint32_t end) {
  const uint64_t a = addr;
  const uint64_t b = (uint64_t)addr + len;

  return len != 0U && a < end && b > start;
}

/* Reads that pop a FIFO would break the USB itself */
bool proto_read_allowed(uint32_t addr, uint32_t len) {
  static const uint32_t s_ranges[][2] = {
    {0x42040020U, 0x42040024U}, /* OTG GRXSTSP */
    {0x42041000U, 0x42060000U}, /* OTG FIFOs */
    {0x52040020U, 0x52040024U},
    {0x52041000U, 0x52060000U},
  };
  if ((uint64_t)addr + len > 0x100000000ULL) {
    return false;
  }
  for (size_t i = 0U; i < sizeof(s_ranges) / sizeof(s_ranges[0]); i++) {
    if (prv_overlaps(addr, len, s_ranges[i][0], s_ranges[i][1])) {
      return false;
    }
  }
  return true;
}

/*
 * Never the flash or the system memory (code region), the flash controller
 * (option bytes), the stub's own RAM or the USB controller.
 */
bool proto_write_allowed(uint32_t addr, uint32_t len) {
  static const uint32_t s_ranges[][2] = {
    {0x00000000U, 0x10000000U},
    {TETHER_STUB_RAM, TETHER_STUB_RAM + TETHER_STUB_RAM_SIZE},
    {0x30000000U, 0x30000000U + TETHER_STUB_RAM_SIZE},
    {0x40022000U, 0x40022400U},
    {0x50022000U, 0x50022400U},
    {0x42040000U, 0x42080000U},
    {0x52040000U, 0x52080000U},
  };
  if ((uint64_t)addr + len > 0x100000000ULL) {
    return false;
  }
  for (size_t i = 0U; i < sizeof(s_ranges) / sizeof(s_ranges[0]); i++) {
    if (prv_overlaps(addr, len, s_ranges[i][0], s_ranges[i][1])) {
      return false;
    }
  }
  return true;
}

static bool prv_width_ok(unsigned width, uint32_t addr, uint32_t len) {
  if (width == 0U || width == 1U) {
    return true;
  }
  if (width != 2U && width != 4U) {
    return false;
  }
  return (addr % width) == 0U && (len % width) == 0U;
}

static void prv_fault_reply(uint8_t cmd, uint8_t seq, int status, const struct tether_fault *f) {
  uint32_t n = 0U;

  if (status == TETHER_ERR_FAULT) {
    memcpy(REPLY_DATA, f, sizeof(*f));
    n = sizeof(*f);
  }
  prv_reply(cmd, seq, status, n);
}

/* Writes to the PMIC, the fuel gauge and the USB-PD controller, beyond a register address */
static bool prv_i2c_protected(int bus, uint8_t addr, uint32_t wn) {
  return bus == 1 && wn > 1U && (addr == 0x61U || addr == 0x36U || addr == 0x29U);
}

static void prv_dispatch(uint8_t cmd, uint8_t seq, const uint8_t *p, uint32_t n) {
  struct tether_fault fault;
  enum port_action action = PORT_ACTION_NONE;
  uint32_t action_arg = 0U;
  uint32_t addr = n >= 4U ? prv_u32(p) : 0U;
  uint32_t len = 0U;
  int st = TETHER_OK;

  memset(&fault, 0, sizeof(fault));

  switch (cmd) {
    case TETHER_CMD_INFO: {
      struct tether_info info;
      memset(&info, 0, sizeof(info));
      port_info(&info);
      memcpy(REPLY_DATA, &info, sizeof(info));
      prv_reply(cmd, seq, TETHER_OK, sizeof(info));
      break;
    }

    case TETHER_CMD_READ: {
      if (n < 9U) {
        st = TETHER_ERR_ARG;
      } else {
        len = prv_u32(p + 4);
        unsigned width = p[8];
        if (len > TETHER_MAX_DATA) {
          st = TETHER_ERR_LENGTH;
        } else if (!prv_width_ok(width, addr, len)) {
          st = TETHER_ERR_ARG;
        } else if (!proto_read_allowed(addr, len)) {
          st = TETHER_ERR_PROTECTED;
        } else {
          st = port_read(addr, REPLY_DATA, len, width, &fault);
        }
      }
      if (st == TETHER_OK) {
        prv_reply(cmd, seq, st, len);
      } else {
        prv_fault_reply(cmd, seq, st, &fault);
      }
      break;
    }

    case TETHER_CMD_WRITE:
      if (n < 8U) {
        st = TETHER_ERR_ARG;
      } else {
        len = n - 8U;
        unsigned width = p[4];
        if (!prv_width_ok(width, addr, len)) {
          st = TETHER_ERR_ARG;
        } else if (!proto_write_allowed(addr, len)) {
          st = TETHER_ERR_PROTECTED;
        } else {
          st = port_write(addr, p + 8, len, width, &fault);
        }
      }
      prv_fault_reply(cmd, seq, st, &fault);
      break;

    case TETHER_CMD_FILL:
      if (n < 16U) {
        st = TETHER_ERR_ARG;
      } else {
        unsigned width = p[4];
        uint32_t value = prv_u32(p + 8);
        uint32_t count = prv_u32(p + 12);
        if (width != 1U && width != 2U && width != 4U) {
          st = TETHER_ERR_ARG;
        } else if ((uint64_t)count * width > 0x01000000ULL) {
          st = TETHER_ERR_LENGTH;
        } else {
          len = count * width;
          if (!prv_width_ok(width, addr, len)) {
            st = TETHER_ERR_ARG;
          } else if (!proto_write_allowed(addr, len)) {
            st = TETHER_ERR_PROTECTED;
          } else {
            st = port_fill(addr, width, value, count, &fault);
          }
        }
      }
      prv_fault_reply(cmd, seq, st, &fault);
      break;

    case TETHER_CMD_CRC32: {
      uint32_t crc = 0U;
      if (n < 8U) {
        st = TETHER_ERR_ARG;
      } else {
        len = prv_u32(p + 4);
        if (!proto_read_allowed(addr, len)) {
          st = TETHER_ERR_PROTECTED;
        } else {
          st = port_crc32(addr, len, &crc, &fault);
        }
      }
      if (st == TETHER_OK) {
        prv_put32(REPLY_DATA, crc);
        prv_reply(cmd, seq, st, 4U);
      } else {
        prv_fault_reply(cmd, seq, st, &fault);
      }
      break;
    }

    case TETHER_CMD_CALL: {
      struct tether_call_result res;
      uint32_t args[TETHER_CALL_MAX_ARGS] = {0};
      memset(&res, 0, sizeof(res));
      if (n < 16U) {
        st = TETHER_ERR_ARG;
        prv_reply(cmd, seq, st, 0U);
        break;
      }
      uint32_t flags = prv_u32(p + 4);
      uint32_t timeout = prv_u32(p + 8);
      uint32_t nargs = prv_u32(p + 12);
      if (nargs > TETHER_CALL_MAX_ARGS || n < 16U + 4U * nargs) {
        prv_reply(cmd, seq, TETHER_ERR_ARG, 0U);
        break;
      }
      for (uint32_t i = 0U; i < nargs; i++) {
        args[i] = prv_u32(p + 16U + 4U * i);
      }
      if ((addr & 1U) == 0U ||
          ((flags & TETHER_CALL_ANYWHERE) == 0U &&
           ((addr & ~1U) < TETHER_LOAD_BASE || (addr & ~1U) >= TETHER_LOAD_END))) {
        prv_reply(cmd, seq, TETHER_ERR_PROTECTED, 0U);
        break;
      }
      len = addr;
      st = port_call(addr, flags, timeout, args, nargs, &res, &fault);
      uint32_t out_len;
      const uint8_t *out = port_out(&out_len);
      const uint32_t room = REPLY_MAX - sizeof(res) - sizeof(fault);
      uint32_t send = out_len < room ? out_len : room;
      res.out_len = out_len;
      /* A timeout comes with where the blob was */
      res.faulted = (st == TETHER_ERR_FAULT || st == TETHER_ERR_TIMEOUT) ? 1U : 0U;
      memcpy(REPLY_DATA, &res, sizeof(res));
      memcpy(REPLY_DATA + sizeof(res), out, send);
      uint32_t total = sizeof(res) + send;
      if (res.faulted) {
        memcpy(REPLY_DATA + total, &fault, sizeof(fault));
        total += sizeof(fault);
      }
      prv_reply(cmd, seq, st, total);
      break;
    }

    case TETHER_CMD_I2C: {
      if (n < 8U) {
        st = TETHER_ERR_ARG;
        prv_reply(cmd, seq, st, 0U);
        break;
      }
      int bus = p[0];
      uint8_t dev = p[1];
      uint8_t flags = p[2];
      uint32_t wn = prv_u16(p + 4);
      uint32_t rn = prv_u16(p + 6);
      addr = dev;
      len = wn + rn;
      if (wn > TETHER_I2C_MAX || rn > TETHER_I2C_MAX || n < 8U + wn) {
        st = TETHER_ERR_LENGTH;
      } else if (prv_i2c_protected(bus, dev, wn) && (flags & TETHER_I2C_UNSAFE) == 0U) {
        st = TETHER_ERR_PROTECTED;
      } else {
        st = port_i2c(bus, dev, p + 8, wn, REPLY_DATA, rn, (flags & TETHER_I2C_400K) != 0U);
      }
      prv_reply(cmd, seq, st, st == TETHER_OK ? rn : 0U);
      break;
    }

    case TETHER_CMD_GPIO: {
      uint32_t regs[8] = {0};
      uint8_t level = 0U;
      if (n < 4U) {
        st = TETHER_ERR_ARG;
      } else {
        unsigned port = p[0], pin = p[1], op = p[2];
        addr = port;
        len = pin;
        if (port > 8U || pin > 15U || op > TETHER_GPIO_ANALOG) {
          st = TETHER_ERR_ARG;
        } else if (op != TETHER_GPIO_READ && port == 0U && pin >= 11U && pin <= 14U) {
          /* USB and SWD */
          st = TETHER_ERR_PROTECTED;
        } else {
          st = port_gpio(port, pin, op, p[3], regs, &level);
        }
      }
      if (st == TETHER_OK) {
        memcpy(REPLY_DATA, regs, sizeof(regs));
        REPLY_DATA[sizeof(regs)] = level;
        prv_reply(cmd, seq, st, sizeof(regs) + 1U);
      } else {
        prv_reply(cmd, seq, st, 0U);
      }
      break;
    }

    case TETHER_CMD_RESET:
      if (n < 1U || p[0] > TETHER_RESET_POWER_OFF) {
        st = TETHER_ERR_ARG;
      } else {
        action = p[0] == TETHER_RESET_POWER_OFF ? PORT_ACTION_POWER_OFF : PORT_ACTION_RESET;
      }
      prv_reply(cmd, seq, st, 0U);
      break;

    case TETHER_CMD_FLASH_ERASE:
      len = n >= 8U ? prv_u32(p + 4) : 0U;
      st = n < 8U ? TETHER_ERR_ARG : port_flash_erase(addr, len);
      prv_reply(cmd, seq, st, 0U);
      break;

    case TETHER_CMD_FLASH_WRITE:
      len = n >= 4U ? n - 4U : 0U;
      st = n < 4U ? TETHER_ERR_ARG : port_flash_write(addr, p + 4, len);
      prv_reply(cmd, seq, st, 0U);
      break;

    case TETHER_CMD_BOOT:
      if (n < 4U) {
        st = TETHER_ERR_ARG;
      } else {
        if (addr == 0U) {
          addr = TETHER_SLOT_BASE;
        }
        st = addr < TETHER_SLOT_BASE ? TETHER_ERR_PROTECTED : port_boot_check(addr);
      }
      if (st == TETHER_OK) {
        action = PORT_ACTION_BOOT;
        action_arg = addr;
      }
      prv_reply(cmd, seq, st, 0U);
      break;

    case TETHER_CMD_NOTE: {
      char text[32];
      uint32_t k = n < sizeof(text) - 1U ? n : sizeof(text) - 1U;
      memcpy(text, p, k);
      text[k] = '\0';
      port_note(text);
      prv_reply(cmd, seq, TETHER_OK, 0U);
      break;
    }

    case TETHER_CMD_CLEAR_FAULT:
      port_clear_fault();
      prv_reply(cmd, seq, TETHER_OK, 0U);
      break;

    default:
      st = TETHER_ERR_CMD;
      prv_reply(cmd, seq, st, 0U);
      break;
  }

  port_activity(cmd, st, addr, len);
  if (action != PORT_ACTION_NONE) {
    port_action(action, action_arg);
  }
}

/* Drops n bytes, then whatever comes before the next magic */
static void prv_drop(uint32_t n) {
  uint32_t k = n;

  while (k < s_pos &&
         !(s_in[k] == TETHER_MAGIC0 && (k + 1U == s_pos || s_in[k + 1U] == TETHER_MAGIC1))) {
    k++;
  }
  if (k >= s_pos) {
    s_pos = 0U;
    return;
  }
  memmove(s_in, &s_in[k], s_pos - k);
  s_pos -= k;
}

/*
 * Runs the frames in what came in. A frame with a bad length or CRC may have
 * been a false start: what follows its magic is looked at again. One that
 * holds no other magic was damaged, and gets a BAD_FRAME reply.
 */
static void prv_advance(void) {
  while (s_pos >= TETHER_HDR_LEN) {
    const uint32_t len = prv_u16(&s_in[4]);
    if (len > TETHER_MAX_PAYLOAD) {
      prv_drop(1U);
      continue;
    }
    const uint32_t need = TETHER_HDR_LEN + len + TETHER_CRC_LEN;
    if (s_pos < need) {
      return;
    }
    const uint32_t crc = prv_u32(&s_in[TETHER_HDR_LEN + len]);
    if (tether_crc32(0U, &s_in[2], 4U + len) == crc) {
      prv_dispatch(s_in[2], s_in[3], &s_in[TETHER_HDR_LEN], len);
      prv_drop(need);
      continue;
    }
    const uint8_t seq = s_in[3];
    prv_drop(1U);
    if (s_pos < 2U) {
      prv_reply(TETHER_CMD_BAD_FRAME, seq, TETHER_ERR_CRC, 0U);
      port_activity(TETHER_CMD_BAD_FRAME, TETHER_ERR_CRC, 0U, 0U);
    }
  }
}

void proto_input(const uint8_t *data, size_t len) {
  for (size_t i = 0U; i < len; i++) {
    const uint8_t b = data[i];

    if (s_pos == 0U && b != TETHER_MAGIC0) {
      continue;
    }
    if (s_pos == 1U && b != TETHER_MAGIC1) {
      s_pos = (b == TETHER_MAGIC0) ? 1U : 0U;
      continue;
    }
    s_in[s_pos++] = b;
    prv_advance();
  }
  s_last_ms = port_millis();
}

/* A frame left half received: what came after its magic may hold another */
void proto_poll(void) {
  if (s_pos != 0U && port_millis() - s_last_ms > TETHER_RX_TIMEOUT_MS) {
    prv_drop(1U);
    prv_advance();
    s_last_ms = port_millis();
  }
}
