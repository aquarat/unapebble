/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Entry from the DTM's HCI parser, in place of its test for a vendor packet: r0 the packet, r1
 * its type. A vendor packet returns to the parser's vendor branch; an ACL packet goes to
 * acl_patch_tx; then, as for any other type, on to the parser's return.
 */

  .syntax unified
  .cpu cortex-m0
  .thumb
  .section .text.entry, "ax"
  .global acl_patch_dispatch
  .thumb_func
acl_patch_dispatch:
  cmp r1, #255
  beq 1f
  cmp r1, #2
  bne 2f
  bl acl_patch_tx
2:
  ldr r0, =PARSER_RETURN
  bx r0
1:
  bx lr
  .ltorg
