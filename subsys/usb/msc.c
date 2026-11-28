/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// Mass storage over the bulk-only transport (USB Mass Storage Class, Bulk-Only Transport 1.0),
// with the SCSI commands hosts send a removable, write-protected disk (SPC-4, SBC-3). No endpoint
// is ever halted: data the device does not have is sent as zeros, data it does not want is taken
// and dropped, and the residue in the status says how much of it counted, as the specification
// allows.

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include <pbl/drivers/udc.h>
#include <pbl/kernel/compiler.h>
#include <pbl/kernel/sem.h>
#include <pbl/kernel/thread.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/usb/msc.h>
#include <pbl/usb/usbd.h>
#include <pbl/util/byteorder.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

PBL_LOG_MODULE_DEFINE(usb_msc, CONFIG_USB_MSC_LOG_LEVEL);

#define EP_OUT 0x01U
#define EP_IN  (PBL_UDC_EP_IN | 0x01U)

#define MSC_CLASS        0x08U
#define MSC_SUBCLASS     0x06U
#define MSC_PROTOCOL_BOT 0x50U

#define REQ_GET_MAX_LUN 0xfeU
#define REQ_RESET       0xffU

#define CBW_SIGNATURE 0x43425355U
#define CSW_SIGNATURE 0x53425355U
#define CBW_LEN       31U
#define CSW_LEN       13U
#define CBW_DIR_IN    0x80U

#define CSW_PASSED      0U
#define CSW_FAILED      1U
#define CSW_PHASE_ERROR 2U

#define BLOCK 512U
#define BUF   CONFIG_USB_MSC_BUF_SIZE

// SCSI operation codes
#define SCSI_TEST_UNIT_READY      0x00U
#define SCSI_REQUEST_SENSE        0x03U
#define SCSI_READ6                0x08U
#define SCSI_WRITE6               0x0aU
#define SCSI_INQUIRY              0x12U
#define SCSI_MODE_SENSE6          0x1aU
#define SCSI_START_STOP_UNIT      0x1bU
#define SCSI_PREVENT_ALLOW        0x1eU
#define SCSI_READ_FORMAT_CAPACITY 0x23U
#define SCSI_READ_CAPACITY10      0x25U
#define SCSI_READ10               0x28U
#define SCSI_WRITE10              0x2aU
#define SCSI_VERIFY10             0x2fU
#define SCSI_SYNCHRONIZE_CACHE10  0x35U
#define SCSI_MODE_SENSE10         0x5aU
#define SCSI_READ16               0x88U
#define SCSI_WRITE16              0x8aU
#define SCSI_VERIFY16             0x8fU
#define SCSI_SYNCHRONIZE_CACHE16  0x91U
#define SCSI_SERVICE_ACTION_IN16  0x9eU
#define SCSI_READ12               0xa8U
#define SCSI_WRITE12              0xaaU
#define SCSI_SA_READ_CAPACITY16   0x10U

// Sense keys, and their additional codes
#define SENSE_NONE            0x00U
#define SENSE_NOT_READY       0x02U
#define SENSE_MEDIUM_ERROR    0x03U
#define SENSE_ILLEGAL_REQUEST 0x05U
#define SENSE_UNIT_ATTENTION  0x06U
#define SENSE_DATA_PROTECT    0x07U

#define ASC_BECOMING_READY  0x0401U
#define ASC_UNRECOVERED     0x1100U
#define ASC_INVALID_OPCODE  0x2000U
#define ASC_LBA_RANGE       0x2100U
#define ASC_INVALID_FIELD   0x2400U
#define ASC_WRITE_PROTECTED 0x2700U
#define ASC_MEDIUM_CHANGED  0x2800U
#define ASC_NO_MEDIUM       0x3a00U

#define STACK_SIZE 4096U
#define WAIT_MS    1000U

struct PBL_PACKED prv_desc {
  struct pbl_usb_desc_iface iface;
  struct pbl_usb_desc_ep out;
  struct pbl_usb_desc_ep in;
};

static const struct prv_desc s_desc = {
  .iface =
      {
        .bLength = sizeof(struct pbl_usb_desc_iface),
        .bDescriptorType = PBL_USB_DESC_INTERFACE,
        .bInterfaceNumber = 0U,
        .bNumEndpoints = 2U,
        .bInterfaceClass = MSC_CLASS,
        .bInterfaceSubClass = MSC_SUBCLASS,
        .bInterfaceProtocol = MSC_PROTOCOL_BOT,
        .iInterface = 1U,
      },
  .out =
      {
        .bLength = sizeof(struct pbl_usb_desc_ep),
        .bDescriptorType = PBL_USB_DESC_ENDPOINT,
        .bEndpointAddress = EP_OUT,
        .bmAttributes = PBL_UDC_EP_BULK,
        .wMaxPacketSize = PBL_CPU_TO_LE16(64U),
      },
  .in = {
    .bLength = sizeof(struct pbl_usb_desc_ep),
    .bDescriptorType = PBL_USB_DESC_ENDPOINT,
    .bEndpointAddress = EP_IN,
    .bmAttributes = PBL_UDC_EP_BULK,
    .wMaxPacketSize = PBL_CPU_TO_LE16(64U),
  },
};

static const char *const s_strings[] = {CONFIG_USB_MSC_NAME};

PBL_USBD_FUNCTION_DECLARE(msc);

static struct {
  const struct pbl_usb_msc_medium *medium;
  atomic_bool configured;
  // Bumped by a configuration and a bulk-only reset: a command under way ends
  atomic_uint gen;
  atomic_int in_ret;
  atomic_int out_ret;
  atomic_bool changed;
  atomic_uint last_read_ms;
  // Whether the host saw no medium last, to tell it one came
  bool absent;
  uint8_t sense_key;
  uint16_t sense_asc;
  // The command
  uint32_t tag;
  uint32_t host_len;
  bool host_in;
  uint8_t cb[16];
  // Bytes of the data stage moved, and of them those of the command, not padding
  uint32_t moved;
  uint32_t relevant;
  uint8_t cbw[BLOCK] PBL_ALIGNED(PBL_UDC_BUF_ALIGN);
  uint8_t csw[CSW_LEN + 3U] PBL_ALIGNED(PBL_UDC_BUF_ALIGN);
  uint8_t buf[2][BUF] PBL_ALIGNED(PBL_UDC_BUF_ALIGN);
} s;

static PBL_SEM_DEFINE(s_in_sem, 0, 1);
static PBL_SEM_DEFINE(s_out_sem, 0, 1);
static PBL_SEM_DEFINE(s_cfg_sem, 0, 1);
PBL_THREAD_STACK_DEFINE(s_stack, STACK_SIZE);
static struct pbl_thread s_thread;
static atomic_bool s_started;

static uint8_t prv_ep(uint8_t ep) {
  return pbl_usbd_ep(PBL_USBD_FUNCTION(msc), ep);
}

static void prv_configured(bool on) {
  atomic_store(&s.configured, on);
  atomic_fetch_add(&s.gen, 1U);
  pbl_sem_give(&s_cfg_sem);
}

static void prv_xfer_done(uint8_t ep, int ret) {
  if (ep == EP_IN) {
    atomic_store(&s.in_ret, ret);
    pbl_sem_give(&s_in_sem);
  } else if (ep == EP_OUT) {
    atomic_store(&s.out_ret, ret);
    pbl_sem_give(&s_out_sem);
  }
}

// What the endpoints run ends, aborted, and the thread starts over on a new command
static void prv_reset(void) {
  const uint16_t mps = pbl_usbd_bulk_mps();

  atomic_fetch_add(&s.gen, 1U);
  (void)pbl_udc_ep_disable(UDC, prv_ep(EP_IN));
  (void)pbl_udc_ep_disable(UDC, prv_ep(EP_OUT));
  (void)pbl_udc_ep_enable(UDC, prv_ep(EP_IN), PBL_UDC_EP_BULK, mps);
  (void)pbl_udc_ep_enable(UDC, prv_ep(EP_OUT), PBL_UDC_EP_BULK, mps);
}

static int prv_class_request(const struct pbl_usb_setup *setup, uint8_t **data, uint16_t *len) {
  static uint8_t s_max_lun;

  if ((setup->bmRequestType & PBL_USB_TYPE_MASK) != PBL_USB_TYPE_CLASS) {
    return -ENOTSUP;
  }

  switch (setup->bRequest) {
    case REQ_GET_MAX_LUN:
      s_max_lun = 0U;
      *data = &s_max_lun;
      *len = 1U;
      return 0;
    case REQ_RESET:
      prv_reset();
      *len = 0U;
      return 0;
    default:
      return -ENOTSUP;
  }
}

PBL_USBD_FUNCTION_DEFINE(msc) = {
  .desc = &s_desc,
  .desc_len = sizeof(s_desc),
  .strings = s_strings,
  .num_strings = PBL_ARRAY_SIZE(s_strings),
  .configured = prv_configured,
  .class_request = prv_class_request,
  .xfer_done = prv_xfer_done,
};

// A transfer queued, and waited for: its bytes, or a negative errno when the host reset, left
// or went quiet
static int prv_start(bool in, void *buf, uint32_t len) {
  pbl_sem_reset(in ? &s_in_sem : &s_out_sem);

  return pbl_udc_ep_queue(UDC, prv_ep(in ? EP_IN : EP_OUT), buf, len);
}

static int prv_wait(bool in, unsigned int gen) {
  for (;;) {
    if (pbl_sem_take(in ? &s_in_sem : &s_out_sem, PBL_MSEC(WAIT_MS)) == 0) {
      return atomic_load(in ? &s.in_ret : &s.out_ret);
    }
    if (!atomic_load(&s.configured) || atomic_load(&s.gen) != gen) {
      return -ECONNABORTED;
    }
  }
}

static int prv_xfer(bool in, void *buf, uint32_t len, unsigned int gen) {
  const int ret = prv_start(in, buf, len);

  return ret != 0 ? ret : prv_wait(in, gen);
}

static void prv_sense(uint8_t key, uint16_t asc) {
  s.sense_key = key;
  s.sense_asc = asc;
}

// What a data-in stage still owes the host, as zeros, unless a short packet ended it; what a
// data-out one has, taken and dropped
static int prv_finish_data(unsigned int gen) {
  if (s.host_in && s.moved % pbl_usbd_bulk_mps() != 0U) {
    return 0;
  }
  if (s.host_in && s.moved < s.host_len) {
    memset(s.buf[0], 0, PBL_MIN(s.host_len - s.moved, (uint32_t)BUF));
  }
  while (s.moved < s.host_len) {
    const uint32_t n = PBL_MIN(s.host_len - s.moved, (uint32_t)BUF);
    const int ret = prv_xfer(s.host_in, s.buf[0], n, gen);

    if (ret < 0) {
      return ret;
    }
    s.moved += n;
    if (!s.host_in && (uint32_t)ret < n) {
      break;
    }
  }

  return 0;
}

// A reply of the command, as much as the host takes
static int prv_reply(const void *data, uint32_t len, unsigned int gen) {
  int ret;

  if (!s.host_in) {
    return -EPROTO;
  }
  len = PBL_MIN(len, s.host_len);
  if (len == 0U) {
    return 0;
  }
  memcpy(s.buf[1], data, len);
  ret = prv_xfer(true, s.buf[1], len, gen);
  if (ret < 0) {
    return ret;
  }
  s.moved = len;
  s.relevant = len;

  return 0;
}

// The medium, if ready: 0, else a negative errno, the sense set
static int prv_ready(uint32_t *num_blocks) {
  const struct pbl_usb_msc_medium *m = s.medium;
  int ret = m != nullptr ? m->status(num_blocks, m->ctx) : -ENOMEDIUM;

  if (ret == 0 && s.absent) {
    s.absent = false;
    atomic_store(&s.changed, true);
  }
  if (ret == 0 && atomic_exchange(&s.changed, false)) {
    prv_sense(SENSE_UNIT_ATTENTION, ASC_MEDIUM_CHANGED);
    return -EAGAIN;
  }
  if (ret == -EAGAIN) {
    prv_sense(SENSE_NOT_READY, ASC_BECOMING_READY);
    return ret;
  }
  if (ret != 0) {
    s.absent = true;
    atomic_store(&s.changed, false);
    prv_sense(SENSE_NOT_READY, ASC_NO_MEDIUM);
    return -ENOMEDIUM;
  }

  return 0;
}

static void prv_put_be(uint8_t *p, uint64_t v, unsigned int bytes) {
  for (unsigned int i = 0U; i < bytes; i++) {
    p[i] = (uint8_t)(v >> (8U * (bytes - 1U - i)));
  }
}

static uint64_t prv_get_be(const uint8_t *p, unsigned int bytes) {
  uint64_t v = 0U;

  for (unsigned int i = 0U; i < bytes; i++) {
    v = (v << 8) | p[i];
  }

  return v;
}

static void prv_pad(char *dst, const char *src, size_t len) {
  size_t n = 0U;

  for (; n < len && src[n] != '\0'; n++) {
    dst[n] = src[n];
  }
  memset(&dst[n], ' ', len - n);
}

static int prv_inquiry(unsigned int gen) {
  uint8_t r[36] = {0};
  const uint32_t alloc = (uint32_t)prv_get_be(&s.cb[3], 2);

  if ((s.cb[1] & 0x01U) != 0U) {
    // Vital product data: the list of pages, which has only itself
    if (s.cb[2] != 0x00U) {
      prv_sense(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD);
      return -EINVAL;
    }
    r[3] = 1U;
    return prv_reply(r, PBL_MIN(5U, alloc), gen);
  }

  r[1] = 0x80U; // removable; a direct-access block device
  r[2] = 0x04U; // SPC-2
  r[3] = 0x02U;
  r[4] = sizeof(r) - 5U;
  prv_pad((char *)&r[8], CONFIG_USB_MSC_VENDOR, 8);
  prv_pad((char *)&r[16], CONFIG_USB_MSC_PRODUCT, 16);
  prv_pad((char *)&r[32], "1.0", 4);

  return prv_reply(r, PBL_MIN((uint32_t)sizeof(r), alloc), gen);
}

static int prv_request_sense(unsigned int gen) {
  uint8_t r[18] = {0};

  r[0] = 0x70U;
  r[2] = s.sense_key;
  r[7] = sizeof(r) - 8U;
  r[12] = (uint8_t)(s.sense_asc >> 8);
  r[13] = (uint8_t)s.sense_asc;
  prv_sense(SENSE_NONE, 0U);

  return prv_reply(r, PBL_MIN((uint32_t)sizeof(r), s.cb[4]), gen);
}

// The header alone, write-protected: no page the host could change
static int prv_mode_sense(bool ten, unsigned int gen) {
  uint8_t r[8] = {0};

  if (ten) {
    r[1] = 6U;
    r[3] = 0x80U;
    return prv_reply(r, PBL_MIN(8U, (uint32_t)prv_get_be(&s.cb[7], 2)), gen);
  }
  r[0] = 3U;
  r[2] = 0x80U;

  return prv_reply(r, PBL_MIN(4U, (uint32_t)s.cb[4]), gen);
}

static int prv_capacity(uint8_t op, unsigned int gen) {
  uint8_t r[32] = {0};
  uint32_t num_blocks;
  int ret = prv_ready(&num_blocks);

  if (ret != 0) {
    return ret;
  }

  switch (op) {
    case SCSI_READ_CAPACITY10:
      prv_put_be(r, num_blocks - 1U, 4);
      prv_put_be(&r[4], BLOCK, 4);
      return prv_reply(r, 8U, gen);
    case SCSI_SERVICE_ACTION_IN16:
      prv_put_be(r, num_blocks - 1U, 8);
      prv_put_be(&r[8], BLOCK, 4);
      return prv_reply(r, PBL_MIN(32U, (uint32_t)prv_get_be(&s.cb[10], 4)), gen);
    default:
      // READ FORMAT CAPACITIES: the list's header, then the medium, formatted
      r[3] = 8U;
      prv_put_be(&r[4], num_blocks, 4);
      r[8] = 0x02U;
      prv_put_be(&r[9], BLOCK, 3);
      return prv_reply(r, PBL_MIN(12U, (uint32_t)prv_get_be(&s.cb[7], 2)), gen);
  }
}

// Blocks of the medium, in pieces: one buffer is read while the other goes to the host
static int prv_read(uint64_t lba, uint32_t count, unsigned int gen) {
  const struct pbl_usb_msc_medium *m = s.medium;
  const uint32_t per = BUF / BLOCK;
  uint32_t num_blocks;
  uint32_t blocks;
  unsigned int cur = 0U;
  bool queued = false;
  int ret = prv_ready(&num_blocks);

  if (ret != 0) {
    return ret;
  }
  if (lba > num_blocks || count > num_blocks - lba) {
    prv_sense(SENSE_ILLEGAL_REQUEST, ASC_LBA_RANGE);
    return -EINVAL;
  }
  if (!s.host_in || (uint64_t)count * BLOCK > s.host_len) {
    return -EPROTO;
  }

  atomic_store(&s.last_read_ms, (uint32_t)pbl_uptime_ms());
  for (blocks = count; blocks > 0U;) {
    const uint32_t n = PBL_MIN(blocks, per);

    ret = m->read((uint32_t)lba, s.buf[cur], n, m->ctx);
    if (ret == 0 && queued) {
      ret = prv_wait(true, gen);
      ret = ret < 0 ? ret : 0;
      queued = false;
    }
    if (ret == 0) {
      ret = prv_start(true, s.buf[cur], n * BLOCK);
      queued = ret == 0;
    }
    if (ret != 0) {
      PBL_LOG_WRN("Read of %" PRIu32 "+%" PRIu32 " failed: %d", (uint32_t)lba, n, ret);
      prv_sense(SENSE_MEDIUM_ERROR, ASC_UNRECOVERED);
      break;
    }
    s.moved += n * BLOCK;
    lba += n;
    blocks -= n;
    cur ^= 1U;
  }
  if (queued) {
    const int done = prv_wait(true, gen);

    if (done < 0) {
      return done;
    }
  }
  s.relevant = s.moved;
  atomic_store(&s.last_read_ms, (uint32_t)pbl_uptime_ms());

  return ret != 0 ? -EIO : 0;
}

// A command, its data stage as far as it goes: 0, or a negative errno with the sense set;
// -EPROTO when the host's direction or length does not fit it, -ECONNABORTED when the host left
static int prv_command(unsigned int gen) {
  const uint8_t op = s.cb[0];
  uint32_t num_blocks;
  int ret;

  switch (op) {
    case SCSI_INQUIRY:
      return prv_inquiry(gen);
    case SCSI_REQUEST_SENSE:
      return prv_request_sense(gen);
    default:
      break;
  }

  // A medium that changed is the first thing the host hears of, but for the above
  if (atomic_load(&s.changed) || s.absent) {
    ret = prv_ready(&num_blocks);
    if (ret != 0) {
      return ret;
    }
  }

  switch (op) {
    case SCSI_TEST_UNIT_READY:
      return prv_ready(&num_blocks);
    case SCSI_MODE_SENSE6:
      return prv_mode_sense(false, gen);
    case SCSI_MODE_SENSE10:
      return prv_mode_sense(true, gen);
    case SCSI_READ_CAPACITY10:
    case SCSI_READ_FORMAT_CAPACITY:
      return prv_capacity(op, gen);
    case SCSI_SERVICE_ACTION_IN16:
      if ((s.cb[1] & 0x1fU) != SCSI_SA_READ_CAPACITY16) {
        break;
      }
      return prv_capacity(op, gen);
    case SCSI_READ6:
      return prv_read(prv_get_be(&s.cb[1], 3) & 0x1fffffU, s.cb[4] != 0U ? s.cb[4] : 256U, gen);
    case SCSI_READ10:
      return prv_read(prv_get_be(&s.cb[2], 4), (uint32_t)prv_get_be(&s.cb[7], 2), gen);
    case SCSI_READ12:
      return prv_read(prv_get_be(&s.cb[2], 4), (uint32_t)prv_get_be(&s.cb[6], 4), gen);
    case SCSI_READ16:
      return prv_read(prv_get_be(&s.cb[2], 8), (uint32_t)prv_get_be(&s.cb[10], 4), gen);
    case SCSI_START_STOP_UNIT:
      // Eject: loading the medium again is the drive's to decide
      if ((s.cb[4] & 0x03U) == 0x02U && s.medium != nullptr && s.medium->eject != nullptr) {
        s.medium->eject(s.medium->ctx);
      }
      return 0;
    case SCSI_PREVENT_ALLOW:
    case SCSI_VERIFY10:
    case SCSI_VERIFY16:
    case SCSI_SYNCHRONIZE_CACHE10:
    case SCSI_SYNCHRONIZE_CACHE16:
      return prv_ready(&num_blocks);
    case SCSI_WRITE6:
    case SCSI_WRITE10:
    case SCSI_WRITE12:
    case SCSI_WRITE16:
      ret = prv_ready(&num_blocks);
      if (ret == 0) {
        prv_sense(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED);
        ret = -EROFS;
      }
      return ret;
    default:
      break;
  }

  PBL_LOG_DBG("SCSI 0x%02x not supported", op);
  prv_sense(SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE);

  return -ENOTSUP;
}

static bool prv_cbw_valid(int len) {
  return len == (int)CBW_LEN && pbl_get_le32(s.cbw) == CBW_SIGNATURE && s.cbw[13] == 0U &&
         s.cbw[14] >= 1U && s.cbw[14] <= 16U;
}

static void prv_thread(void *arg) {
  for (;;) {
    unsigned int gen;
    uint8_t status;
    int ret;

    if (!atomic_load(&s.configured)) {
      (void)pbl_sem_take(&s_cfg_sem, PBL_MSEC(WAIT_MS));
      continue;
    }
    gen = atomic_load(&s.gen);

    ret = prv_xfer(false, s.cbw, CBW_LEN, gen);
    if (ret < 0) {
      continue;
    }
    // A command block that is none: the host resets the transport once it times out
    if (!prv_cbw_valid(ret)) {
      PBL_LOG_WRN("Not a command block (%d bytes)", ret);
      continue;
    }

    s.tag = pbl_get_le32(&s.cbw[4]);
    s.host_len = pbl_get_le32(&s.cbw[8]);
    s.host_in = (s.cbw[12] & CBW_DIR_IN) != 0U;
    memset(s.cb, 0, sizeof(s.cb));
    memcpy(s.cb, &s.cbw[15], s.cbw[14]);
    s.moved = 0U;
    s.relevant = 0U;

    ret = prv_command(gen);
    if (ret == -ECONNABORTED || atomic_load(&s.gen) != gen) {
      continue;
    }
    status = ret == 0 ? CSW_PASSED : (ret == -EPROTO ? CSW_PHASE_ERROR : CSW_FAILED);
    if (prv_finish_data(gen) < 0) {
      continue;
    }

    pbl_put_le32(CSW_SIGNATURE, s.csw);
    pbl_put_le32(s.tag, &s.csw[4]);
    pbl_put_le32(s.host_len - s.relevant, &s.csw[8]);
    s.csw[12] = status;
    (void)prv_xfer(true, s.csw, CSW_LEN, gen);
  }
}

void pbl_usb_msc_init(const struct pbl_usb_msc_medium *medium) {
  s.medium = medium;
  if (atomic_exchange(&s_started, true)) {
    return;
  }

  const struct pbl_thread_attr attr = {
    .name = "USB drive",
    .entry = prv_thread,
    .prio = CONFIG_USB_MSC_THREAD_PRIO,
    .privileged = true,
    .stack = s_stack,
    .stack_size = sizeof(s_stack),
  };
  if (pbl_thread_create(&s_thread, &attr) != 0) {
    PBL_LOG_ERR("No USB drive thread");
  }
}

void pbl_usb_msc_changed(void) {
  atomic_store(&s.changed, true);
}

bool pbl_usb_msc_is_configured(void) {
  return atomic_load(&s.configured);
}

uint32_t pbl_usb_msc_last_read_ms(void) {
  return atomic_load(&s.last_read_ms);
}
