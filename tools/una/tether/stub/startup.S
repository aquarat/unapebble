/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Vectors, reset, and the guard: guard_run() calls a function on its own stack
 * (PSP); a fault or a timeout taken from it unwinds to guard_run's caller
 * with an error code instead of killing the stub. The exception handler
 * builds an exception frame on the main stack where guard_run left it and
 * returns to guard_recover through it.
 */

        .syntax unified
        .cpu cortex-m33
        .thumb

        .section .isr_vector, "a", %progbits
        .global vectors_flash
        .type vectors_flash, %object
vectors_flash:
        .word _estack
        .word Reset_Handler
        .word fault_entry       /* NMI: flash ECC double error */
        .word fault_entry       /* HardFault */
        .word fault_entry       /* MemManage */
        .word fault_entry       /* BusFault */
        .word fault_entry       /* UsageFault */
        .word fault_entry       /* SecureFault */
        .word 0, 0, 0
        .word fault_entry       /* SVCall */
        .word fault_entry       /* DebugMon */
        .word 0
        .word fault_entry       /* PendSV */
        .word SysTick_Handler
        .rept 141
        .word fault_entry       /* IRQs: an unexpected one is disabled and recorded */
        .endr
        .size vectors_flash, . - vectors_flash

        .text

        .global Reset_Handler
        .type Reset_Handler, %function
        .thumb_func
Reset_Handler:
        cpsid i
        ldr r0, =_estack
        msr msp, r0
        movs r0, #0
        msr control, r0
        isb
        /* .data from flash */
        ldr r0, =_sdata
        ldr r1, =_edata
        ldr r2, =_sidata
1:      cmp r0, r1
        bhs 2f
        ldr r3, [r2], #4
        str r3, [r0], #4
        b 1b
2:      /* .bss */
        ldr r0, =_sbss
        ldr r1, =_ebss
        movs r3, #0
3:      cmp r0, r1
        bhs 4f
        str r3, [r0], #4
        b 3b
4:      /* FPU: CP10 and CP11 full access, for blobs */
        ldr r0, =0xe000ed88
        ldr r1, [r0]
        orr r1, r1, #(0xf << 20)
        str r1, [r0]
        dsb
        isb
        bl main
5:      b 5b
        .size Reset_Handler, . - Reset_Handler

/*
 * Every fault, and unexpected exceptions and IRQs. fault_handle() records the
 * fault and returns 0 to carry on (an IRQ it disabled), a code to unwind the
 * guard with, or does not return (a fault outside the guard resets).
 */
        .global fault_entry
        .type fault_entry, %function
        .thumb_func
fault_entry:
        mov r2, lr
        tst r2, #4
        ite eq
        mrseq r0, msp
        mrsne r0, psp
        mrs r3, ipsr
        push {r4-r11}
        mov r1, sp
        push {r2, r3}
        bl fault_handle
        pop {r2, r3}
        add sp, sp, #32
        cbz r0, 1f
        b guard_abort
1:      bx r2
        .size fault_entry, . - fault_entry

        .global SysTick_Handler
        .type SysTick_Handler, %function
        .thumb_func
SysTick_Handler:
        push {r0, lr}
        mov r0, lr
        bl systick_tick
        pop {r1, r2}
        cbz r0, 1f
        b guard_abort
1:      bx r2
        .size SysTick_Handler, . - SysTick_Handler

/*
 * r0: the code guard_run returns, r2: EXC_RETURN. Only a guarded thread mode
 * context can be unwound; anything else is fatal.
 */
        .type guard_abort, %function
        .thumb_func
guard_abort:
        ldr r3, =g_guard
        ldr r1, [r3]
        cbz r1, 9f
        tst r2, #8
        beq 9f
        /* A lazy FP context of the abandoned stack is dropped */
        ldr r3, =0xe000ef34
        ldr r12, [r3]
        bic r12, r12, #1
        str r12, [r3]
        sub r1, r1, #32
        str r0, [r1, #0]
        movs r0, #0
        str r0, [r1, #4]
        str r0, [r1, #8]
        str r0, [r1, #12]
        str r0, [r1, #16]
        str r0, [r1, #20]
        ldr r0, =guard_recover
        bic r0, r0, #1
        str r0, [r1, #24]
        mov r0, #0x01000000
        str r0, [r1, #28]
        msr msp, r1
        /* Privileged thread mode, on the main stack, with a basic frame */
        mrs r0, control
        bic r0, r0, #1
        msr control, r0
        bic r2, r2, #4
        orr r2, r2, #0x10
        isb
        bx r2
9:      mov r0, r2
        bl fault_fatal
        b .
        .size guard_abort, . - guard_abort

/*
 * int guard_run(uint32_t (*fn)(uint32_t, uint32_t, uint32_t, uint32_t),
 *               const uint32_t args[4], uint32_t psp_top, uint32_t ret[2])
 * Returns 0 with fn's r0 and r1 in ret, or the code of the abort.
 */
        .global guard_run
        .type guard_run, %function
        .thumb_func
guard_run:
        push {r3-r11, lr}
        ldr r12, =g_guard
        str sp, [r12]
        mov r4, r0
        msr psp, r2
        mov r12, r1
        ldm r12, {r0-r3}
        mrs r12, control
        orr r12, r12, #2
        msr control, r12
        isb
        blx r4
        mrs r12, control
        bic r12, r12, #2
        msr control, r12
        isb
        ldr r12, =g_guard
        movs r2, #0
        str r2, [r12]
        ldr r3, [sp]
        stm r3, {r0, r1}
        movs r0, #0
        pop {r3-r11, pc}
        .size guard_run, . - guard_run

        .type guard_recover, %function
        .thumb_func
guard_recover:
        ldr r12, =g_guard
        movs r1, #0
        str r1, [r12]
        cpsie i
        pop {r3-r11, pc}
        .size guard_recover, . - guard_recover

/*
 * void boot_jump(uint32_t sp, uint32_t pc): to an image as the bootloader
 * starts one, interrupts masked, on the main stack.
 */
        .global boot_jump
        .type boot_jump, %function
        .thumb_func
boot_jump:
        cpsid i
        movs r2, #0
        msr control, r2
        isb
        msr msp, r0
        orr r1, r1, #1
        bx r1
        .size boot_jump, . - boot_jump
