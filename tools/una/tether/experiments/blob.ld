/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/* An experiment: linked at the stub's load region, its header first */

MEMORY
{
    RAM (rwx) : ORIGIN = 0x20020000, LENGTH = 0x2024e000 - 0x20020000
}

ENTRY(blob_start)

SECTIONS
{
    .header : {
        KEEP(*(.blob_header))
    } > RAM

    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > RAM

    .data : {
        *(.data .data.*)
        . = ALIGN(4);
    } > RAM

    _image_end = .;

    /* Not in the image: blob_start clears it */
    .bss (NOLOAD) : {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /DISCARD/ : {
        *(.ARM.attributes)
        *(.ARM.exidx*)
    }
}
