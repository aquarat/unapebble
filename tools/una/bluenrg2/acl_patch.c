/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Code added to ST's BlueNRG-2 DTM v3.2.0 (SPI, build 2353) so that it passes ACL data between
 * its link layer and the host in link-layer-only mode, which that build does not: its HCI parser
 * drops the host's ACL packets, and the stack's callback for received ACL data resolves to a
 * function that does nothing. patch.py places this code in the DTM's unused flash and redirects
 * the two places to it; the addresses below are of functions and data in that one build, found
 * by analysis, and patch.py refuses any other image.
 *
 * Built for the Cortex-M0, position-dependent at ACL_PATCH_BASE.
 */

#include <stdint.h>

/* The stack's context; at +0xa4, bit 0: link-layer-only mode */
#define STACK_CTX          (*(uint8_t *volatile *)0x20005384U)
#define CTX_LL_ONLY_OFFSET 0xa4U

#define FN(addr) ((uintptr_t)(addr) | 1U)

/* The connection's index from its handle */
#define get_connection_no ((uint8_t (*)(uint16_t, uint8_t))FN(0x1004e49c))
/* A packet buffer for a connection, of a payload length */
#define pm_get_pckt ((uint8_t *(*)(uint32_t, uint16_t, uint16_t))FN(0x10051e60))
/* Data into a packet buffer, at an offset */
#define pm_write_pckt_data \
  ((void (*)(uint8_t *, uint16_t, uint16_t, const uint8_t *))FN(0x10055610))
/* A packet queued to the link layer; non-zero if it was not */
#define acl_transmit_data ((uint32_t (*)(uint8_t *))FN(0x10056aa0))
/* A packet buffer given back */
#define pm_release_acl_pckt ((void (*)(uint8_t *, uint32_t))FN(0x100525bc))
/* A packet queued to the host over SPI */
#define send_event_isr ((void (*)(uint8_t *, uint16_t, int32_t))FN(0x100477ac))

#define ACL_MAX  251U
#define CONN_MAX 8U

/* Packet buffer fields, as the stack's own users set them */
#define PKT_LEN      6U
#define PKT_PB       12U
#define PKT_FLAGS    19U
#define PB_START     2U
#define PB_CONTINUED 1U
/* Completion reported to the host (Number Of Completed Packets) */
#define FLAG_FROM_HOST 1U

/* An H4 ACL packet from the host, its type byte first */
void acl_patch_tx(const uint8_t *h4) {
  const uint16_t handle = (uint16_t)(h4[1] | ((h4[2] & 0x0fU) << 8));
  const uint8_t pb = (uint8_t)((h4[2] >> 4) & 3U);
  const uint16_t len = (uint16_t)(h4[3] | (h4[4] << 8));
  uint8_t *pkt;

  if ((STACK_CTX[CTX_LL_ONLY_OFFSET] & 1U) == 0U || handle > 0x0effU || len == 0U ||
      len > ACL_MAX || get_connection_no(handle, 3U) >= CONN_MAX) {
    return;
  }

  pkt = pm_get_pckt(0U, handle, len);
  if (pkt == 0) {
    return;
  }
  pkt[PKT_LEN] = (uint8_t)len;
  pkt[PKT_LEN + 1U] = (uint8_t)(len >> 8);
  pm_write_pckt_data(pkt, 0U, len, &h4[5]);
  pkt[PKT_PB] = (pb & 1U) != 0U ? PB_CONTINUED : PB_START;
  pkt[PKT_FLAGS] = FLAG_FROM_HOST;
  if (acl_transmit_data(pkt) != 0U) {
    pm_release_acl_pckt(pkt, 0U);
  }
}

/* ACL data from the link layer, to the host as an H4 packet; called where the stack's callback
   did nothing, with its arguments */
uint32_t acl_patch_rx(uint16_t handle, uint8_t pb, uint8_t bc, uint16_t len, const uint8_t *pdu) {
  uint8_t out[5U + ACL_MAX];

  if (len > ACL_MAX) {
    len = ACL_MAX;
  }
  out[0] = 0x02U;
  out[1] = (uint8_t)handle;
  out[2] = (uint8_t)(((handle >> 8) & 0x0fU) | ((pb & 3U) << 4) | ((bc & 3U) << 6));
  out[3] = (uint8_t)len;
  out[4] = (uint8_t)(len >> 8);
  for (uint16_t i = 0U; i < len; i++) {
    out[5U + i] = pdu[i];
  }
  send_event_isr(out, (uint16_t)(5U + len), -1);

  return 0U;
}
