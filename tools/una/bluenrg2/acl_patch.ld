/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

ENTRY(acl_patch_dispatch)
SECTIONS {
  . = ACL_PATCH_BASE;
  .text : {
    *(.text.entry)
    *(.text*)
    *(.rodata*)
  }
  /DISCARD/ : { *(.data*) *(.bss*) *(.ARM.*) *(.comment) }
}
