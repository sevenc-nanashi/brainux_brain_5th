// Registers: r0=staged Image, r1=Image bytes, r2=staged DTB, r3=DTB bytes.
// All four arguments are physical. No stack or literal-pool accesses after entry.
.section .text.handoff,"ax"
.arm
.balign 32
.global handoff
.type handoff,%function
handoff:
    cpsid if
    mov r8, r0
    mov r11, r1
    mov r12, r2
    mov lr, r3
    // Walk every data/unified cache level, from the CPU out to memory.
    dmb sy
    mrc p15, 1, r0, c0, c0, 1
    ubfx r3, r0, #24, #3
    lsl r3, r3, #1
    mov r10, #0
1:
    cmp r10, r3
    bge 5f
    add r2, r10, r10, lsr #1
    lsr r1, r0, r2
    and r1, r1, #7
    cmp r1, #2
    blt 4f
    mcr p15, 2, r10, c0, c0, 0
    isb
    mrc p15, 1, r1, c0, c0, 0
    and r2, r1, #7
    add r2, r2, #4
    ubfx r4, r1, #3, #10
    clz r5, r4
    ubfx r6, r1, #13, #15
2:
    mov r7, r4
3:
    lsl r9, r7, r5
    orr r9, r9, r6, lsl r2
    orr r9, r9, r10
    mcr p15, 0, r9, c7, c14, 2
    subs r7, r7, #1
    bge 3b
    subs r6, r6, #1
    bge 2b
    dsb sy
4:
    add r10, r10, #2
    b 1b
5:
    mov r0, #0
    mcr p15, 2, r0, c0, c0, 0
    dsb sy
    // Own short-descriptor table: executable identity RAM plus the app alias.
    // Firmware identity aliases may be XN, even when ATS1CPR succeeds.
    mov r1, #0x67000000
    add r1, r1, #0x00600000
    mcr p15, 0, r0, c2, c0, 2
    mcr p15, 0, r1, c2, c0, 0
    mcr p15, 0, r1, c2, c0, 1
    mov r1, #1
    mcr p15, 0, r1, c3, c0, 0
    isb
    mcr p15, 0, r0, c8, c7, 0
    dsb sy
    isb
    // Execute through the verified identity alias before disabling translation.
    adr r1, 6f
    sub r1, r1, #0x90000000
    add r1, r1, #0x67000000
    add r1, r1, #0x00100000
    bx r1
6:
    mrc p15, 0, r1, c1, c0, 0
    bic r1, r1, #1
    bic r1, r1, #4
    bic r1, r1, #0x1000
    bic r1, r1, #0x800
    dsb sy
    mcr p15, 0, r1, c1, c0, 0
    isb
    mcr p15, 0, r0, c8, c7, 0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c7, c5, 6
    dsb sy
    isb
    // Copy byte-exactly: neither input length needs word alignment.
    movw r4, #0x8000
    movt r4, #0x6100
    mov r5, r4
7:
    ldrb r6, [r8], #1
    strb r6, [r5], #1
    subs r11, r11, #1
    bne 7b
    mov r2, #0x67000000
    mov r5, r2
8:
    ldrb r6, [r12], #1
    strb r6, [r5], #1
    subs lr, lr, #1
    bne 8b
    dsb sy
    // Linux owns SVC's stack from here; do not use the firmware's banked stack.
    cps #0x13
    mov r0, #0
    mvn r1, #0
    bx r4
.size handoff, .-handoff
