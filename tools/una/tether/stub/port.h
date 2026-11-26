/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * What proto.c needs from where it runs: port.c on the watch, sim_port.c on a
 * host for the tests. Memory accesses return TETHER_ERR_FAULT with *fault
 * filled in when they fault.
 */

#pragma once

#include "tether_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum port_action {
  PORT_ACTION_NONE,
  PORT_ACTION_RESET,
  PORT_ACTION_POWER_OFF,
  PORT_ACTION_BOOT,
};

uint32_t port_millis(void);
/* Queues a reply; may drop it if nobody reads */
void port_send(const uint8_t *data, size_t len);

void port_info(struct tether_info *info);
int port_read(uint32_t addr, uint8_t *dst, uint32_t len, unsigned width,
              struct tether_fault *fault);
int port_write(uint32_t addr, const uint8_t *src, uint32_t len, unsigned width,
               struct tether_fault *fault);
int port_fill(uint32_t addr, unsigned width, uint32_t value, uint32_t count,
              struct tether_fault *fault);
int port_crc32(uint32_t addr, uint32_t len, uint32_t *crc, struct tether_fault *fault);
int port_call(uint32_t addr, uint32_t flags, uint32_t timeout_ms, const uint32_t *args,
              uint32_t nargs, struct tether_call_result *res, struct tether_fault *fault);
const uint8_t *port_out(uint32_t *len);
int port_i2c(int bus, uint8_t addr, const uint8_t *wr, uint32_t wn, uint8_t *rd, uint32_t rn,
             bool fast);
int port_gpio(unsigned port, unsigned pin, unsigned op, unsigned arg, uint32_t regs[8],
              uint8_t *level);
int port_flash_erase(uint32_t addr, uint32_t len);
int port_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);
/* TETHER_OK if a vector table at addr looks bootable */
int port_boot_check(uint32_t addr);
void port_note(const char *text);
void port_clear_fault(void);
void port_activity(uint8_t cmd, int status, uint32_t addr, uint32_t len);
/* Once the reply has gone */
void port_action(enum port_action action, uint32_t arg);

/* proto.c */
uint32_t tether_crc32(uint32_t crc, const void *data, size_t len);
void proto_input(const uint8_t *data, size_t len);
void proto_poll(void);
bool proto_read_allowed(uint32_t addr, uint32_t len);
bool proto_write_allowed(uint32_t addr, uint32_t len);
