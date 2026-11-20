/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ARMv4T implementation of the 64-byte, unsalted expansion in OpenBSD's
 * Blowfish. bcrypt passes SHA-512 digests to this operation. */
.syntax unified
.arch armv4t
.arm
.text
.align 2

/* r0-r3 hold the four S-box bases, r4 the subkeys, and r7 the byte-offset mask.
 * The two halves stay in r5 and r6 for the entire expansion. */
.macro round dst, src, index
    mov r10, \src, lsr #24
    and r11, r7, \src, lsr #14
    and r12, r7, \src, lsr #6
    ldr r10, [r0, r10, lsl #2]
    ldr r11, [r1, r11]
    ldr r12, [r2, r12]
    add r10, r10, r11
    and r11, \src, #255
    ldr r11, [r3, r11, lsl #2]
    eor r10, r10, r12
    ldr r12, [r4, #(4 * \index)]
    add r10, r10, r11
    eor \dst, \dst, r12
    eor \dst, \dst, r10
.endm

.global pt_blowfish_expand64
.type pt_blowfish_expand64, %function
pt_blowfish_expand64:
    push {r4-r11, lr}
    add r4, r0, #4096

    /* XOR the 18 subkeys with big-endian words, wrapping the 64-byte digest. */
    mov r8, #0
.Lkey:
    and r12, r8, #60
    add r12, r1, r12
    ldrb r5, [r12]
    ldrb r6, [r12, #1]
    ldrb r10, [r12, #2]
    ldrb r11, [r12, #3]
    orr r6, r6, r5, lsl #8
    orr r10, r10, r6, lsl #8
    orr r11, r11, r10, lsl #8
    ldr r5, [r4, r8]
    eor r5, r5, r11
    str r5, [r4, r8]
    add r8, r8, #4
    cmp r8, #72
    bne .Lkey

    add r1, r0, #1024
    add r2, r0, #2048
    add r3, r0, #3072
    mov r5, #0
    mov r6, #0
    mov r7, #1020

    /* Replace P first. Each encryption sees the subkeys already updated by
     * preceding pairs. Continue the same chain into all four S-boxes. */
    mov r8, r4
    add r9, r4, #72
.Lsubkeys:
    bl .Lencipher
    stmia r8!, {r5, r6}
    cmp r8, r9
    bne .Lsubkeys

    mov r8, r0
.Lboxes:
    bl .Lencipher
    stmia r8!, {r5, r6}
    cmp r8, r4
    bne .Lboxes

    pop {r4-r11, lr}
    bx lr

.Lencipher:
    ldr r10, [r4]
    eor r5, r5, r10
    round r6, r5, 1
    round r5, r6, 2
    round r6, r5, 3
    round r5, r6, 4
    round r6, r5, 5
    round r5, r6, 6
    round r6, r5, 7
    round r5, r6, 8
    round r6, r5, 9
    round r5, r6, 10
    round r6, r5, 11
    round r5, r6, 12
    round r6, r5, 13
    round r5, r6, 14
    round r6, r5, 15
    round r5, r6, 16
    ldr r10, [r4, #68]
    mov r11, r5
    eor r5, r6, r10
    mov r6, r11
    bx lr
.size pt_blowfish_expand64, .-pt_blowfish_expand64

.section .note.GNU-stack,"",%progbits
