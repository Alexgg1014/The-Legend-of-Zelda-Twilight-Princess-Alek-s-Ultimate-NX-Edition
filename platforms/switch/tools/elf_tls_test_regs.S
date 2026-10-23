// TLS-lab: tls_regcheck(), linked into the TLS test modules. It fills x2-x29 and all 128 bits of
// q0-q31 with known values, makes one TLS descriptor call exactly as GCC emits it, and returns 0 if
// every one of them survived, or the number of the first that did not (x: 2..29, q: 100+n). x0
// (the result), x1 (the scratch that held the resolver), x30 and the flags may change: that is what
// gcc/config/aarch64/aarch64.md (tlsdesc_small_advsimd) lets the call clobber.
    .text
    .global tls_regcheck
    .type   tls_regcheck, %function
    .p2align 4
tls_regcheck:
    stp     x29, x30, [sp, #-96]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    stp     d8, d9, [sp, #-64]!
    stp     d10, d11, [sp, #16]
    stp     d12, d13, [sp, #32]
    stp     d14, d15, [sp, #48]

    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    mov     x1, #(0x2000 + \r)
    mov     v\r\().d[0], x1
    mov     x1, #(0x3000 + \r)
    mov     v\r\().d[1], x1
    .endr
    .irp r, 2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
    mov     x\r, #(0x1000 + \r)
    .endr

    adrp    x0, :tlsdesc:t_probe
    ldr     x1, [x0, #:tlsdesc_lo12:t_probe]
    add     x0, x0, #:tlsdesc_lo12:t_probe
    .tlsdesccall t_probe
    blr     x1

    .irp r, 2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
    mov     x1, #(0x1000 + \r)
    cmp     x\r, x1
    mov     x0, #\r
    b.ne    9f
    .endr
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    mov     x1, v\r\().d[0]
    mov     x2, #(0x2000 + \r)
    cmp     x1, x2
    mov     x0, #(100 + \r)
    b.ne    9f
    mov     x1, v\r\().d[1]
    mov     x2, #(0x3000 + \r)
    cmp     x1, x2
    b.ne    9f
    .endr
    mov     x0, #0
9:  ldp     d14, d15, [sp, #48]
    ldp     d12, d13, [sp, #32]
    ldp     d10, d11, [sp, #16]
    ldp     d8, d9, [sp], #64
    ldp     x27, x28, [sp, #80]
    ldp     x25, x26, [sp, #64]
    ldp     x23, x24, [sp, #48]
    ldp     x21, x22, [sp, #32]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #96
    ret
    .size   tls_regcheck, . - tls_regcheck
    .section .note.GNU-stack, "", %progbits
