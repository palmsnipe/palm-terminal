.syntax unified
.arm
.section .text.arm_entry,"ax",%progbits
.global arm_entry
.type arm_entry,%function
arm_entry:
    stmfd sp!, {r4, lr}
    mov r4, sp
    mov r0, r1
    ldrb r2, [r0, #0]
    ldrb r3, [r0, #1]
    orr r2, r2, r3, lsl #8
    ldrb r3, [r0, #2]
    orr r2, r2, r3, lsl #16
    ldrb r3, [r0, #3]
    orr sp, r2, r3, lsl #24
    bl pt_arm_dispatch
    mov sp, r4
    ldmfd sp!, {r4, lr}
    bx lr
.size arm_entry, .-arm_entry

.text
.align 2
.global pt_set_return
.type pt_set_return,%function
pt_set_return:
    stmia r0, {r4-r11, sp, lr}
    mov r0, #0
    bx lr
.size pt_set_return, .-pt_set_return

.global pt_fault_return
.type pt_fault_return,%function
pt_fault_return:
    ldmia r0, {r4-r11, sp, lr}
    mov r0, #1
    bx lr
.size pt_fault_return, .-pt_fault_return

.section .note.GNU-stack,"",%progbits
