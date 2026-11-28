/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/*
 * The SPI protocol of an ST BlueNRG-2 running ST's DTM firmware (BlueNRG-2N data sheet DS13280,
 * 6.5), and the chip's start-up, for the transports that drive it: the HCI one
 * (NIMBLE_HCI_BLUENRG2) and the ACI one (NIMBLE_ACI_BLUENRG2). One thread does every transaction.
 */

#include <stddef.h>
#include <stdint.h>

#include <pbl/bluetooth/id_addr.h>

/* aci_blue_initialized_event, the vendor event a start begins with, and its reasons */
#define BNRG_VS_BLUE_INITIALIZED 0x0001U
#define BNRG_START_REASON_NORMAL 0x01U

/* The packet types of the protocol: H4 ones */
#define BNRG_PKT_CMD 0x01U
#define BNRG_PKT_ACL 0x02U
#define BNRG_PKT_EVT 0x04U

/** Sets the IRQ up; the chip stays in reset. */
void bnrg_spi_init(void);

/** IRQ is high: the chip has something to read. */
bool bnrg_irq_high(void);

/** Waits until IRQ rises or bnrg_kick() is called, whichever is first. */
void bnrg_wait(void);

/** Wakes the thread in bnrg_wait(). Any thread. */
void bnrg_kick(void);

/** One read, while IRQ is high: the bytes read, or a negative errno. */
int bnrg_read(uint8_t *buf, uint32_t size);

/**
 * One write of a whole packet, retried while the chip has no room or is not ready, for up to a
 * second. Whatever the chip has meanwhile goes to rx_cb, which reads it.
 */
int bnrg_write(const uint8_t *buf, uint32_t len, void (*rx_cb)(void));

/**
 * Restarts the chip until it starts normally and the setup succeeds, trying again from a long
 * reset; gives up only for a while, never for good. first: the chip has been in reset since its
 * device came up. setup runs the start's own commands, with bnrg_init_cmd().
 */
void bnrg_bring_up(bool first, int (*setup)(void));

/** Holds the chip in reset, its chip select undriven. */
void bnrg_hold_reset(void);

/**
 * During the setup: a command and its Command Complete, the other packets dropped. Its status,
 * or a negative errno; the rest of the return parameters go to ret, up to *ret_len.
 */
int bnrg_init_cmd(uint16_t op, const void *par, uint8_t par_len, void *ret, size_t *ret_len);

/**
 * During the setup: the module's static random address, at config 0x80, made one by its two top
 * bits (Core specification, Vol 6, Part B, 1.3.2.1). 0, or -ENOENT if there is none.
 */
int bnrg_read_identity(struct pbl_bt_addr *addr);
