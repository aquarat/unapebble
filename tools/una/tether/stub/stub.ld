/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The stub runs where the UNA bootloader starts images; the PebbleOS slot
 * starts at 0x08080000, after it. Its RAM is the first 128 KiB; blobs load
 * after it.
 */

MEMORY
{
    FLASH (rx) : ORIGIN = 0x08060000, LENGTH = 128K
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 128K
}

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);
_stack_size = 16K;

SECTIONS
{
    .isr_vector : {
        KEEP(*(.isr_vector))
    } > FLASH

    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx : {
        *(.ARM.exidx*)
    } > FLASH

    _sidata = LOADADDR(.data);

    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* Not cleared: the framebuffer and the buffers, set up before use */
    .noinit (NOLOAD) : {
        . = ALIGN(1024);
        *(.ram_vectors)
        *(.noinit .noinit.*)
    } > RAM

    ASSERT(. <= _estack - _stack_size, "the stub's RAM overflows into its stack")

    /DISCARD/ : {
        *(.ARM.attributes)
    }
}
