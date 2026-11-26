/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * A polled CDC-ACM device on the USB OTG HS controller (Synopsys DWC2) and its
 * internal high-speed PHY, in slave (FIFO) mode, with no interrupts. The PHY
 * and core bring-up follow ST's HAL for the STM32U5A5 (stm32u5xx_ll_usb.c,
 * USB_CoreInit and USB_DevInit) and UNA's Usb.cpp: the PHY takes the 16 MHz
 * HSE, the transceiver supply needs voltage range 1 or 2.
 *
 * Endpoints: 0 control, 0x82 interrupt (notifications, never sent), 0x01 and
 * 0x81 bulk data.
 */

#include "regs.h"
#include "stub.h"

#include <string.h>

struct usb_status g_usb;

#define VID 0x1209U /* pid.codes */
#define PID 0x0001U /* its test PID, for private use */

#define EP0_MPS    64U
#define NOTIFY_MPS 16U
#define NUM_EPS    9U

/* FIFO RAM in words, 1024 in all */
#define RX_WORDS  288U
#define TX0_WORDS 32U
#define TX1_WORDS 256U
#define TX2_WORDS 16U

#define RING_SIZE 8192U
struct ring {
  uint8_t buf[RING_SIZE];
  uint32_t head; /* written */
  uint32_t tail; /* read */
};
static struct ring s_rx __attribute__((section(".noinit")));
static struct ring s_tx __attribute__((section(".noinit")));

static uint32_t prv_ring_count(const struct ring *r) {
  return r->head - r->tail;
}
static uint32_t prv_ring_free(const struct ring *r) {
  return RING_SIZE - prv_ring_count(r);
}

static void prv_ring_put(struct ring *r, const uint8_t *d, uint32_t n) {
  for (uint32_t i = 0U; i < n; i++) {
    r->buf[(r->head + i) % RING_SIZE] = d[i];
  }
  r->head += n;
}

static uint32_t prv_ring_get(struct ring *r, uint8_t *d, uint32_t n) {
  uint32_t c = prv_ring_count(r);

  n = n < c ? n : c;
  for (uint32_t i = 0U; i < n; i++) {
    d[i] = r->buf[(r->tail + i) % RING_SIZE];
  }
  r->tail += n;
  return n;
}

static bool s_hs;
static uint16_t s_mps = 64U;
static bool s_configured;
static bool s_ep1_out_armed;
static bool s_ep1_in_busy;
static bool s_need_zlp;
static uint32_t s_last_setup_ms;

static union {
  uint8_t b[8];
  uint32_t w[2];
} s_setup;

static uint8_t s_ep0_buf[96];
static const uint8_t *s_ep0_ptr;
static uint16_t s_ep0_left;
static bool s_ep0_zlp;
static uint8_t s_ep0_out_req;
static uint8_t s_ep0_out[EP0_MPS];
static uint16_t s_ep0_out_len;
static uint8_t s_pkt[512];

/* 115200 8N1, which nothing uses */
static uint8_t s_line_coding[7] = {0x00, 0xc2, 0x01, 0x00, 0x00, 0x00, 0x08};

static bool prv_wait(uintptr_t reg, uint32_t mask, bool set, uint32_t ms) {
  uint32_t start = millis();

  while (((rd(reg) & mask) != 0U) != set) {
    if (millis() - start > ms) {
      return false;
    }
  }
  return true;
}

static void prv_flush_tx(uint32_t num) {
  wr(OTG_GRSTCTL, GRSTCTL_TXFFLSH | (num << GRSTCTL_TXFNUM_SHIFT));
  (void)prv_wait(OTG_GRSTCTL, GRSTCTL_TXFFLSH, false, 10U);
}

static void prv_flush_rx(void) {
  wr(OTG_GRSTCTL, GRSTCTL_RXFFLSH);
  (void)prv_wait(OTG_GRSTCTL, GRSTCTL_RXFFLSH, false, 10U);
}

static void prv_fifo_write(unsigned ep, const uint8_t *src, uint32_t len) {
  volatile uint32_t *fifo = (volatile uint32_t *)(uintptr_t)OTG_FIFO(ep);

  for (uint32_t i = 0U; i < len; i += 4U) {
    uint32_t w = 0U;
    for (uint32_t b = 0U; b < 4U && i + b < len; b++) {
      w |= (uint32_t)src[i + b] << (8U * b);
    }
    *fifo = w;
  }
}

/* Reads a packet of len bytes from the receive FIFO, keeping the first max of them */
static void prv_fifo_read(uint8_t *dst, uint32_t len, uint32_t max) {
  volatile uint32_t *fifo = (volatile uint32_t *)(uintptr_t)OTG_FIFO(0);

  for (uint32_t i = 0U; i < len; i += 4U) {
    uint32_t w = *fifo;
    for (uint32_t b = 0U; b < 4U && i + b < len; b++) {
      if (dst != NULL && i + b < max) {
        dst[i + b] = (uint8_t)(w >> (8U * b));
      }
    }
  }
}

static void prv_in_send(unsigned ep, const uint8_t *data, uint32_t len) {
  wr(OTG_DIEPTSIZ(ep), (1U << EPTSIZ_PKTCNT_SHIFT) | len);
  setb(OTG_DIEPCTL(ep), EPCTL_EPENA | EPCTL_CNAK);
  if (len != 0U) {
    prv_fifo_write(ep, data, len);
  }
}

/* An enabled endpoint's size must not change: one still armed stays as it is */
static void prv_ep0_out_arm(void) {
  if (rd(OTG_DOEPCTL(0)) & EPCTL_EPENA) {
    return;
  }
  wr(OTG_DOEPTSIZ(0), DOEPTSIZ0_STUPCNT3 | (1U << EPTSIZ_PKTCNT_SHIFT) | EP0_MPS);
  setb(OTG_DOEPCTL(0), EPCTL_EPENA | EPCTL_CNAK);
}

static void prv_ep0_stall(void) {
  setb(OTG_DIEPCTL(0), EPCTL_STALL);
  setb(OTG_DOEPCTL(0), EPCTL_STALL);
}

static void prv_ep0_status(void) {
  prv_in_send(0, NULL, 0U);
}

static void prv_ep0_next(void) {
  if (s_ep0_left != 0U) {
    uint16_t n = s_ep0_left > EP0_MPS ? EP0_MPS : s_ep0_left;
    prv_in_send(0, s_ep0_ptr, n);
    s_ep0_ptr += n;
    s_ep0_left -= n;
  } else if (s_ep0_zlp) {
    s_ep0_zlp = false;
    prv_ep0_status();
  }
}

static void prv_ep0_reply(const uint8_t *data, uint16_t len, uint16_t wlength) {
  if (len > wlength) {
    len = wlength;
  }
  s_ep0_ptr = data;
  s_ep0_left = len;
  s_ep0_zlp = (len < wlength) && (len % EP0_MPS) == 0U;
  /* The status stage, from the host */
  prv_ep0_out_arm();
  if (len == 0U) {
    s_ep0_zlp = false;
    prv_ep0_status();
  } else {
    prv_ep0_next();
  }
}

static const uint8_t s_device_desc[18] = {
  18,      1,           0x00,     0x02,                              /* USB 2.0 */
  0x02,    0x00,        0x00,                                        /* CDC */
  EP0_MPS, VID & 0xffU, VID >> 8, PID & 0xffU, PID >> 8, 0x00, 0x01, /* bcdDevice 1.00 */
  1,       2,           3,        1,
};

static const uint8_t s_qualifier_desc[10] = {10, 6, 0x00, 0x02, 0x02, 0x00, 0x00, EP0_MPS, 1, 0};

#define CONFIG_LEN 67U

static uint16_t prv_config_desc(uint8_t *d, bool hs, uint8_t type) {
  const uint16_t mps = hs ? 512U : 64U;
  uint8_t *p = d;

  const uint8_t config[] = {9, type, CONFIG_LEN, 0, 2, 1, 0, 0x80, 50};
  memcpy(p, config, sizeof(config));
  p += sizeof(config);
  /* Communication interface: ACM, no AT commands */
  const uint8_t comm[] = {9, 4, 0, 0, 1, 0x02, 0x02, 0x00, 0};
  memcpy(p, comm, sizeof(comm));
  p += sizeof(comm);
  const uint8_t functional[] = {
    5, 0x24, 0x00, 0x10, 0x01, /* header, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 0x01, /* call management: data interface 1 */
    4, 0x24, 0x02, 0x02,       /* ACM: line coding and state */
    5, 0x24, 0x06, 0x00, 0x01, /* union: 0 controls 1 */
  };
  memcpy(p, functional, sizeof(functional));
  p += sizeof(functional);
  const uint8_t notify[] = {7, 5, 0x82, 0x03, NOTIFY_MPS, 0, (uint8_t)(hs ? 8U : 16U)};
  memcpy(p, notify, sizeof(notify));
  p += sizeof(notify);
  const uint8_t data[] = {
    9,
    4,
    1,
    0,
    2,
    0x0a,
    0x00,
    0x00,
    0,
    7,
    5,
    0x01,
    0x02,
    (uint8_t)(mps & 0xffU),
    (uint8_t)(mps >> 8),
    0,
    7,
    5,
    0x81,
    0x02,
    (uint8_t)(mps & 0xffU),
    (uint8_t)(mps >> 8),
    0,
  };
  memcpy(p, data, sizeof(data));
  p += sizeof(data);
  return (uint16_t)(p - d);
}

static uint16_t prv_string_desc(uint8_t *d, uint8_t index) {
  static const char *const s_strings[] = {"unapebble", "UNA Watch tether"};
  char serial[25];
  const char *s;

  if (index == 0U) {
    d[0] = 4;
    d[1] = 3;
    d[2] = 0x09;
    d[3] = 0x04;
    return 4U;
  }
  if (index == 3U) {
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0U; i < 12U; i++) {
      uint8_t b = (uint8_t)(rd(UID_BASE + (i / 4U) * 4U) >> ((i % 4U) * 8U));
      serial[i * 2U] = hex[b >> 4];
      serial[i * 2U + 1U] = hex[b & 0xfU];
    }
    serial[24] = '\0';
    s = serial;
  } else if (index <= 2U) {
    s = s_strings[index - 1U];
  } else {
    return 0U;
  }
  uint16_t n = (uint16_t)strlen(s);
  d[0] = (uint8_t)(2U + 2U * n);
  d[1] = 3;
  for (uint16_t i = 0U; i < n; i++) {
    d[2U + 2U * i] = (uint8_t)s[i];
    d[3U + 2U * i] = 0;
  }
  return d[0];
}

static void prv_deconfigure(void) {
  for (unsigned ep = 1U; ep < NUM_EPS; ep++) {
    wr(OTG_DIEPCTL(ep), (rd(OTG_DIEPCTL(ep)) & EPCTL_EPENA) ? (EPCTL_EPDIS | EPCTL_SNAK) : 0U);
    wr(OTG_DOEPCTL(ep), (rd(OTG_DOEPCTL(ep)) & EPCTL_EPENA) ? (EPCTL_EPDIS | EPCTL_SNAK) : 0U);
  }
  s_configured = false;
  s_ep1_out_armed = false;
  s_ep1_in_busy = false;
  s_need_zlp = false;
  g_usb.dtr = false;
}

static void prv_configure(void) {
  prv_deconfigure();
  wr(OTG_DIEPCTL(1), s_mps | (2U << EPCTL_EPTYP_SHIFT) | (1U << EPCTL_TXFNUM_SHIFT) | EPCTL_SD0PID |
                         EPCTL_USBAEP | EPCTL_SNAK);
  wr(OTG_DIEPCTL(2), NOTIFY_MPS | (3U << EPCTL_EPTYP_SHIFT) | (2U << EPCTL_TXFNUM_SHIFT) |
                         EPCTL_SD0PID | EPCTL_USBAEP | EPCTL_SNAK);
  wr(OTG_DOEPCTL(1), s_mps | (2U << EPCTL_EPTYP_SHIFT) | EPCTL_SD0PID | EPCTL_USBAEP | EPCTL_SNAK);
  setb(OTG_DAINTMSK, (1U << 1) | (1U << 2) | (1U << 17));
  prv_flush_tx(1U);
  prv_flush_tx(2U);
  s_configured = true;
  /* Whatever was left from before is stale */
  s_tx.tail = s_tx.head;
}

static void prv_std_request(uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                            uint16_t length) {
  const uint8_t recipient = type & 0x1fU;
  static uint8_t s_two[2];

  switch (req) {
    case 0x00: /* GET_STATUS */
      s_two[0] = 0;
      s_two[1] = 0;
      if (recipient == 2U) {
        unsigned ep = index & 0x0fU;
        uint32_t ctl = (index & 0x80U) ? rd(OTG_DIEPCTL(ep)) : rd(OTG_DOEPCTL(ep));
        s_two[0] = (ctl & EPCTL_STALL) ? 1U : 0U;
      }
      prv_ep0_reply(s_two, 2U, length);
      return;
    case 0x01: /* CLEAR_FEATURE */
    case 0x03: /* SET_FEATURE */
      if (recipient == 2U && value == 0U) {
        unsigned ep = index & 0x0fU;
        uintptr_t ctl = (index & 0x80U) ? OTG_DIEPCTL(ep) : OTG_DOEPCTL(ep);
        if (ep == 0U || ep >= NUM_EPS) {
          break;
        }
        if (req == 0x03U) {
          setb(ctl, EPCTL_STALL);
        } else {
          modb(ctl, EPCTL_STALL, EPCTL_SD0PID);
        }
      }
      prv_ep0_status();
      return;
    case 0x05: /* SET_ADDRESS: taken at once, the core answers the status stage at address 0 */
      modb(OTG_DCFG, DCFG_DAD_MASK, ((uint32_t)value & 0x7fU) << DCFG_DAD_SHIFT);
      prv_ep0_status();
      return;
    case 0x06: { /* GET_DESCRIPTOR */
      uint8_t dtype = (uint8_t)(value >> 8);
      uint16_t n = 0U;
      switch (dtype) {
        case 1:
          prv_ep0_reply(s_device_desc, sizeof(s_device_desc), length);
          return;
        case 2:
          n = prv_config_desc(s_ep0_buf, s_hs, 2U);
          break;
        case 3:
          n = prv_string_desc(s_ep0_buf, (uint8_t)value);
          break;
        case 6:
          prv_ep0_reply(s_qualifier_desc, sizeof(s_qualifier_desc), length);
          return;
        case 7:
          n = prv_config_desc(s_ep0_buf, !s_hs, 7U);
          break;
        default:
          break;
      }
      if (n == 0U) {
        break;
      }
      prv_ep0_reply(s_ep0_buf, n, length);
      return;
    }
    case 0x08: /* GET_CONFIGURATION */
      s_two[0] = s_configured ? 1U : 0U;
      prv_ep0_reply(s_two, 1U, length);
      return;
    case 0x09: /* SET_CONFIGURATION */
      if (value > 1U) {
        break;
      }
      if (value == 1U) {
        prv_configure();
        g_usb.state = TETHER_USB_CONFIGURED;
      } else {
        prv_deconfigure();
        g_usb.state = TETHER_USB_RESET;
      }
      status_changed();
      prv_ep0_status();
      return;
    case 0x0a: /* GET_INTERFACE */
      s_two[0] = 0;
      prv_ep0_reply(s_two, 1U, length);
      return;
    case 0x0b: /* SET_INTERFACE */
      if (value != 0U) {
        break;
      }
      prv_ep0_status();
      return;
    default:
      break;
  }
  prv_ep0_stall();
}

static void prv_class_request(uint8_t req, uint16_t value, uint16_t length) {
  switch (req) {
    case 0x20: /* SET_LINE_CODING: its data comes in the OUT stage */
      s_ep0_out_req = req;
      s_ep0_out_len = 0U;
      prv_ep0_out_arm();
      return;
    case 0x21: /* GET_LINE_CODING */
      prv_ep0_reply(s_line_coding, sizeof(s_line_coding), length);
      return;
    case 0x22: /* SET_CONTROL_LINE_STATE */
      g_usb.dtr = (value & 1U) != 0U;
      if (s_configured) {
        g_usb.state = g_usb.dtr ? TETHER_USB_OPEN : TETHER_USB_CONFIGURED;
        status_changed();
      }
      prv_ep0_status();
      return;
    case 0x23: /* SEND_BREAK */
      prv_ep0_status();
      return;
    default:
      prv_ep0_stall();
      return;
  }
}

static void prv_setup(void) {
  const uint8_t type = s_setup.b[0];
  const uint8_t req = s_setup.b[1];
  const uint16_t value = (uint16_t)(s_setup.b[2] | (s_setup.b[3] << 8));
  const uint16_t index = (uint16_t)(s_setup.b[4] | (s_setup.b[5] << 8));
  const uint16_t length = (uint16_t)(s_setup.b[6] | (s_setup.b[7] << 8));

  s_last_setup_ms = millis();
  /* A new request ends whatever the last one left in the FIFO */
  if ((rd(OTG_DIEPTSIZ(0)) & (3U << EPTSIZ_PKTCNT_SHIFT)) != 0U) {
    prv_flush_tx(0U);
  }
  s_ep0_left = 0U;
  s_ep0_zlp = false;
  s_ep0_out_req = 0U;

  switch (type & 0x60U) {
    case 0x00:
      prv_std_request(type, req, value, index, length);
      break;
    case 0x20:
      prv_class_request(req, value, length);
      break;
    default:
      prv_ep0_stall();
      break;
  }
}

static void prv_ep0_out_done(void) {
  if (s_ep0_out_req == 0x20U) {
    memcpy(s_line_coding, s_ep0_out,
           s_ep0_out_len < sizeof(s_line_coding) ? s_ep0_out_len : sizeof(s_line_coding));
    s_ep0_out_req = 0U;
    prv_ep0_status();
  }
}

static void prv_bus_reset(void) {
  clrb(OTG_DCTL, 1U); /* RWUSIG */
  prv_flush_tx(0x10U);
  for (unsigned ep = 0U; ep < NUM_EPS; ep++) {
    wr(OTG_DIEPINT(ep), 0xfb7fU);
    clrb(OTG_DIEPCTL(ep), EPCTL_STALL);
    wr(OTG_DOEPINT(ep), 0xfb7fU);
    clrb(OTG_DOEPCTL(ep), EPCTL_STALL);
    setb(OTG_DOEPCTL(ep), EPCTL_SNAK);
  }
  prv_deconfigure();
  wr(OTG_DAINTMSK, 0x10001U);
  clrb(OTG_DCFG, DCFG_DAD_MASK);
  /* EP0 takes SETUP packets */
  wr(OTG_DOEPTSIZ(0), DOEPTSIZ0_STUPCNT3 | (1U << EPTSIZ_PKTCNT_SHIFT) | (3U * 8U));
  s_ep0_left = 0U;
  s_ep0_zlp = false;
  s_ep0_out_req = 0U;
  s_rx.tail = s_rx.head;
  s_tx.tail = s_tx.head;
  s_last_setup_ms = millis();
  g_usb.state = TETHER_USB_RESET;
  status_changed();
}

static void prv_enum_done(void) {
  s_hs = DSTS_ENUMSPD(rd(OTG_DSTS)) == 0U;
  s_mps = s_hs ? 512U : 64U;
  g_usb.speed = s_hs ? 2U : 1U;
  /* EP0 IN: 64 bytes */
  clrb(OTG_DIEPCTL(0), 3U);
  setb(OTG_DCTL, DCTL_CGINAK);
  /* Turnaround: 9 PHY clocks at high speed, 6 at full speed with HCLK above 32 MHz */
  modb(OTG_GUSBCFG, GUSBCFG_TRDT_MASK, (s_hs ? 9U : 6U) << GUSBCFG_TRDT_SHIFT);
  status_changed();
}

static void prv_rx_fifo(void) {
  while ((rd(OTG_GINTSTS) & GINTSTS_RXFLVL) != 0U) {
    const uint32_t st = rd(OTG_GRXSTSP);
    const unsigned ep = GRXSTSP_EPNUM(st);
    const uint32_t n = GRXSTSP_BCNT(st);

    switch (GRXSTSP_PKTSTS(st)) {
      case PKTSTS_SETUP_DATA:
        prv_fifo_read(s_setup.b, n, sizeof(s_setup.b));
        break;
      case PKTSTS_OUT_DATA:
        if (ep == 0U) {
          prv_fifo_read(s_ep0_out, n, sizeof(s_ep0_out));
          s_ep0_out_len = (uint16_t)(n < sizeof(s_ep0_out) ? n : sizeof(s_ep0_out));
        } else if (ep == 1U) {
          prv_fifo_read(s_pkt, n, sizeof(s_pkt));
          /* Armed only with room for a packet */
          if (n <= sizeof(s_pkt) && n <= prv_ring_free(&s_rx)) {
            prv_ring_put(&s_rx, s_pkt, n);
          }
        } else {
          prv_fifo_read(NULL, n, 0U);
        }
        break;
      default:
        prv_fifo_read(NULL, n, 0U);
        break;
    }
  }
}

static void prv_ep1_out_arm(void) {
  wr(OTG_DOEPTSIZ(1), (1U << EPTSIZ_PKTCNT_SHIFT) | s_mps);
  setb(OTG_DOEPCTL(1), EPCTL_EPENA | EPCTL_CNAK);
  s_ep1_out_armed = true;
}

static void prv_tx_pump(void) {
  if (!s_configured || s_ep1_in_busy) {
    return;
  }
  uint32_t n = prv_ring_count(&s_tx);
  if (n != 0U) {
    n = prv_ring_get(&s_tx, s_pkt, s_mps);
    prv_in_send(1U, s_pkt, n);
    s_ep1_in_busy = true;
    s_need_zlp = (n == s_mps);
  } else if (s_need_zlp) {
    prv_in_send(1U, NULL, 0U);
    s_ep1_in_busy = true;
    s_need_zlp = false;
  }
}

void usb_poll(void) {
  if (g_usb.state == TETHER_USB_OFF || g_usb.state == TETHER_USB_ERROR) {
    return;
  }

  uint32_t gint = rd(OTG_GINTSTS);

  if (gint & GINTSTS_USBRST) {
    wr(OTG_GINTSTS, GINTSTS_USBRST);
    prv_bus_reset();
  }
  if (gint & GINTSTS_ENUMDNE) {
    wr(OTG_GINTSTS, GINTSTS_ENUMDNE);
    prv_enum_done();
  }
  if (gint & GINTSTS_USBSUSP) {
    wr(OTG_GINTSTS, GINTSTS_USBSUSP);
    g_usb.state = TETHER_USB_SUSPENDED;
    status_changed();
  }
  if (gint & GINTSTS_WKUPINT) {
    wr(OTG_GINTSTS, GINTSTS_WKUPINT);
    g_usb.state =
        s_configured ? (g_usb.dtr ? TETHER_USB_OPEN : TETHER_USB_CONFIGURED) : TETHER_USB_RESET;
    status_changed();
  }

  prv_rx_fifo();

  uint32_t oi = rd(OTG_DOEPINT(0));
  if (oi & EPINT_XFRC) {
    wr(OTG_DOEPINT(0), EPINT_XFRC);
    prv_ep0_out_done();
  }
  if (oi & DOEPINT_STUP) {
    wr(OTG_DOEPINT(0), DOEPINT_STUP | DOEPINT_STPKTRX);
    prv_setup();
  }
  if (oi & ~(EPINT_XFRC | DOEPINT_STUP)) {
    wr(OTG_DOEPINT(0), oi & ~(EPINT_XFRC | DOEPINT_STUP));
  }

  uint32_t ii = rd(OTG_DIEPINT(0));
  if (ii & EPINT_XFRC) {
    wr(OTG_DIEPINT(0), EPINT_XFRC);
    prv_ep0_next();
  }

  if (s_configured) {
    if (rd(OTG_DIEPINT(1)) & EPINT_XFRC) {
      wr(OTG_DIEPINT(1), EPINT_XFRC);
      s_ep1_in_busy = false;
      if (g_usb.state == TETHER_USB_SUSPENDED) {
        g_usb.state = g_usb.dtr ? TETHER_USB_OPEN : TETHER_USB_CONFIGURED;
      }
    }
    if (rd(OTG_DOEPINT(1)) & EPINT_XFRC) {
      wr(OTG_DOEPINT(1), EPINT_XFRC);
      s_ep1_out_armed = false;
    }
    if (!s_ep1_out_armed && prv_ring_free(&s_rx) >= s_mps) {
      prv_ep1_out_arm();
    }
    prv_tx_pump();
  }
}

size_t usb_write(const uint8_t *data, size_t len) {
  uint32_t n = prv_ring_free(&s_tx);

  if (len < n) {
    n = (uint32_t)len;
  }
  prv_ring_put(&s_tx, data, n);
  prv_tx_pump();
  return n;
}

bool usb_tx_idle(void) {
  return prv_ring_count(&s_tx) == 0U && !s_ep1_in_busy && !s_need_zlp;
}

size_t usb_read(uint8_t *data, size_t len) {
  return prv_ring_get(&s_rx, data, (uint32_t)len);
}

bool usb_quiet(uint32_t ms) {
  return millis() - s_last_setup_ms > ms;
}

/* A reply nobody reads: the packet in the FIFO goes, and what is queued */
void usb_tx_abort(void) {
  if (s_ep1_in_busy) {
    setb(OTG_DIEPCTL(1), EPCTL_SNAK);
    (void)prv_wait(OTG_DIEPINT(1), 1U << 6, true, 5U); /* INEPNE */
    setb(OTG_DIEPCTL(1), EPCTL_EPDIS | EPCTL_SNAK);
    (void)prv_wait(OTG_DIEPINT(1), EPINT_EPDISD, true, 5U);
    wr(OTG_DIEPINT(1), 0xfb7fU);
    prv_flush_tx(1U);
  }
  s_ep1_in_busy = false;
  s_need_zlp = false;
  s_tx.tail = s_tx.head;
}

static void prv_fail(uint8_t step) {
  g_usb.error = step;
  g_usb.state = TETHER_USB_ERROR;
  status_changed();
}

void usb_init(bool force_fs) {
  memset(&s_rx, 0, sizeof(s_rx));
  memset(&s_tx, 0, sizeof(s_tx));
  g_usb = (struct usb_status){.state = TETHER_USB_OFF};

  /* The PHY and the core as the bootloader left them, reset: the host sees a disconnect */
  setb(RCC(STM32U5_RCC_APB3ENR_OFFSET), RCC_APB3_SYSCFG);
  (void)rd(RCC(STM32U5_RCC_APB3ENR_OFFSET));
  clrb(SYSCFG_OTGHSPHYCR, SYSCFG_OTGHSPHYCR_EN);
  setb(RCC(RCC_AHB2RSTR1_OFFSET), RCC_AHB2_OTG);
  (void)rd(RCC(RCC_AHB2RSTR1_OFFSET));
  clrb(RCC(RCC_AHB2RSTR1_OFFSET), RCC_AHB2_OTG);

  if (!clock_hse_on()) {
    prv_fail(1U);
    return;
  }

  /* The PHY's reference: HSE at 16 MHz */
  modb(RCC(STM32U5_RCC_CCIPR2_OFFSET), RCC_CCIPR2_USBPHYCSEL_MASK, 0U);
  modb(SYSCFG_OTGHSPHYCR, SYSCFG_OTGHSPHYCR_CLKSEL_MASK, SYSCFG_OTGHSPHYCR_CLKSEL_16MHZ);
  setb(RCC(STM32U5_RCC_AHB2ENR1_OFFSET), RCC_AHB2_OTG | RCC_AHB2_USBPHYC);
  (void)rd(RCC(STM32U5_RCC_AHB2ENR1_OFFSET));
  setb(PWR(STM32U5_PWR_SVMCR_OFFSET), PWR_SVMCR_USV);
  setb(PWR(STM32U5_PWR_VOSR_OFFSET), PWR_VOSR_USBPWREN | PWR_VOSR_USBBOOSTEN);
  delay_ms(2U);
  setb(SYSCFG_OTGHSPHYCR, SYSCFG_OTGHSPHYCR_EN);
  delay_ms(5U);

  /* Core: UTMI to the internal PHY, then a soft reset */
  clrb(OTG_GAHBCFG, GAHBCFG_GINT);
  clrb(OTG_GUSBCFG, GUSBCFG_TSDPS);
  if (!prv_wait(OTG_GRSTCTL, GRSTCTL_AHBIDL, true, 100U)) {
    prv_fail(2U);
    return;
  }
  setb(OTG_GRSTCTL, GRSTCTL_CSRST);
  if (!prv_wait(OTG_GRSTCTL, GRSTCTL_CSRST, false, 100U)) {
    prv_fail(3U);
    return;
  }
  delay_us(10U);

  /* Device mode, which takes 25 ms */
  modb(OTG_GUSBCFG, GUSBCFG_FHMOD | GUSBCFG_FDMOD | GUSBCFG_TRDT_MASK,
       GUSBCFG_FDMOD | (9U << GUSBCFG_TRDT_SHIFT));
  delay_ms(30U);
  if (!prv_wait(OTG_GINTSTS, GINTSTS_CMOD, false, 100U)) {
    prv_fail(4U);
    return;
  }

  for (unsigned i = 1U; i < NUM_EPS; i++) {
    wr(OTG_DIEPTXF(i), 0U);
  }
  clrb(OTG_GCCFG, GCCFG_PULLDOWNEN);
  /* Disconnected until set up; no VBUS sensing, the session is taken as valid */
  setb(OTG_DCTL, DCTL_SDIS);
  clrb(OTG_GCCFG, GCCFG_VBDEN);
  setb(OTG_GCCFG, GCCFG_VBVALEXTOEN | GCCFG_VBVALOVAL);
  wr(OTG_PCGCCTL, 0U);
  modb(OTG_DCFG, DCFG_DSPD_MASK | DCFG_DAD_MASK, force_fs ? DCFG_DSPD_HS_IN_FS : DCFG_DSPD_HS);

  wr(OTG_GRXFSIZ, RX_WORDS);
  wr(OTG_DIEPTXF0, (TX0_WORDS << 16) | RX_WORDS);
  wr(OTG_DIEPTXF(1), (TX1_WORDS << 16) | (RX_WORDS + TX0_WORDS));
  wr(OTG_DIEPTXF(2), (TX2_WORDS << 16) | (RX_WORDS + TX0_WORDS + TX1_WORDS));
  prv_flush_tx(0x10U);
  prv_flush_rx();

  wr(OTG_DIEPMSK, 0U);
  wr(OTG_DOEPMSK, 0U);
  wr(OTG_DAINTMSK, 0U);
  for (unsigned ep = 0U; ep < NUM_EPS; ep++) {
    if (rd(OTG_DIEPCTL(ep)) & EPCTL_EPENA) {
      wr(OTG_DIEPCTL(ep), ep == 0U ? EPCTL_SNAK : (EPCTL_EPDIS | EPCTL_SNAK));
    } else {
      wr(OTG_DIEPCTL(ep), 0U);
    }
    wr(OTG_DIEPTSIZ(ep), 0U);
    wr(OTG_DIEPINT(ep), 0xfb7fU);
    if (rd(OTG_DOEPCTL(ep)) & EPCTL_EPENA) {
      wr(OTG_DOEPCTL(ep), ep == 0U ? EPCTL_SNAK : (EPCTL_EPDIS | EPCTL_SNAK));
    } else {
      wr(OTG_DOEPCTL(ep), 0U);
    }
    wr(OTG_DOEPTSIZ(ep), 0U);
    wr(OTG_DOEPINT(ep), 0xfb7fU);
  }

  /* Polled, but the status bits of the endpoints only reach GINTSTS through the masks */
  wr(OTG_GINTMSK, 0U);
  wr(OTG_GINTSTS, 0xbfffffffU);
  wr(OTG_GINTMSK, GINTSTS_RXFLVL | GINTSTS_USBSUSP | GINTSTS_USBRST | GINTSTS_ENUMDNE |
                      GINTSTS_IEPINT | GINTSTS_OEPINT | GINTSTS_WKUPINT);
  wr(OTG_DIEPMSK, EPINT_XFRC | EPINT_EPDISD | (1U << 3));
  wr(OTG_DOEPMSK, EPINT_XFRC | EPINT_EPDISD | DOEPINT_STUP);
  wr(OTG_DAINTMSK, 0x10001U);

  /* Long enough disconnected for a host that still saw the bootloader's drive */
  delay_ms(300U);
  g_usb.state = TETHER_USB_WAITING;
  s_last_setup_ms = millis();
  clrb(OTG_DCTL, DCTL_SDIS);
  status_changed();
}

/* Before a reset or a jump: the host sees the device go */
void usb_shutdown(void) {
  if (g_usb.state != TETHER_USB_OFF && g_usb.state != TETHER_USB_ERROR) {
    setb(OTG_DCTL, DCTL_SDIS);
    delay_ms(20U);
  }
  clrb(SYSCFG_OTGHSPHYCR, SYSCFG_OTGHSPHYCR_EN);
  setb(RCC(RCC_AHB2RSTR1_OFFSET), RCC_AHB2_OTG);
  (void)rd(RCC(RCC_AHB2RSTR1_OFFSET));
  clrb(RCC(RCC_AHB2RSTR1_OFFSET), RCC_AHB2_OTG);
  clrb(RCC(STM32U5_RCC_AHB2ENR1_OFFSET), RCC_AHB2_OTG | RCC_AHB2_USBPHYC);
  g_usb.state = TETHER_USB_OFF;
}
