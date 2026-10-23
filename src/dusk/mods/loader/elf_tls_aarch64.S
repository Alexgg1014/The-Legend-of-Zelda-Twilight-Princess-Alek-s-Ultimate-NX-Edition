// aleks2 (TLS-lab): the TLS descriptor resolvers for modules mapped by elf_loader (see elf_tls.hpp).
//
// The convention is the descriptor's, not AAPCS64. GCC's tlsdesc_small_advsimd pattern
// (gcc/config/aarch64/aarch64.md) lets the call clobber only x0 (the result), x30, the condition
// flags and the scratch register that held the resolver's address -- which the resolver cannot
// know, so every other general register and all of q0-q31 are preserved here, as glibc's
// _dl_tlsdesc_dynamic does (sysdeps/aarch64/dl-tlsdesc.S). The result is an offset from TPIDR_EL0.
//
// On entry x0 = &descriptor; descriptor[1] is the argument map() stored.

#if defined(__aarch64__)

// The game's own thread pointer, through which its thread-locals (aleks2_tls_dtv) are reached with
// local-exec offsets: on Switch libnx's ThreadVars::tls_tp at TPIDRRO_EL0 + 0x1F8, exactly what
// libnx's __aarch64_read_tp (-mtp=soft) returns; elsewhere (the Linux host tests) TPIDR_EL0.
.macro host_tp reg
#if defined(__SWITCH__)
    mrs     \reg, tpidrro_el0
    ldr     \reg, [\reg, #0x1F8]
#else
    mrs     \reg, tpidr_el0
#endif
.endm

// Saves what an AAPCS64 call may clobber and the descriptor convention does not allow: x5-x18,
// x29, x30 and q0-q31 (x1-x4 are saved by the fast path already). 16 + 112 + 512 bytes.
.macro save_caller_saved
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #(112 + 512)
    stp     x5, x6, [sp, #0]
    stp     x7, x8, [sp, #16]
    stp     x9, x10, [sp, #32]
    stp     x11, x12, [sp, #48]
    stp     x13, x14, [sp, #64]
    stp     x15, x16, [sp, #80]
    stp     x17, x18, [sp, #96]
    stp     q0, q1, [sp, #112]
    stp     q2, q3, [sp, #144]
    stp     q4, q5, [sp, #176]
    stp     q6, q7, [sp, #208]
    stp     q8, q9, [sp, #240]
    stp     q10, q11, [sp, #272]
    stp     q12, q13, [sp, #304]
    stp     q14, q15, [sp, #336]
    stp     q16, q17, [sp, #368]
    stp     q18, q19, [sp, #400]
    stp     q20, q21, [sp, #432]
    stp     q22, q23, [sp, #464]
    stp     q24, q25, [sp, #496]
    stp     q26, q27, [sp, #528]
    stp     q28, q29, [sp, #560]
    stp     q30, q31, [sp, #592]
.endm

.macro restore_caller_saved
    ldp     x5, x6, [sp, #0]
    ldp     x7, x8, [sp, #16]
    ldp     x9, x10, [sp, #32]
    ldp     x11, x12, [sp, #48]
    ldp     x13, x14, [sp, #64]
    ldp     x15, x16, [sp, #80]
    ldp     x17, x18, [sp, #96]
    ldp     q0, q1, [sp, #112]
    ldp     q2, q3, [sp, #144]
    ldp     q4, q5, [sp, #176]
    ldp     q6, q7, [sp, #208]
    ldp     q8, q9, [sp, #240]
    ldp     q10, q11, [sp, #272]
    ldp     q12, q13, [sp, #304]
    ldp     q14, q15, [sp, #336]
    ldp     q16, q17, [sp, #368]
    ldp     q18, q19, [sp, #400]
    ldp     q20, q21, [sp, #432]
    ldp     q22, q23, [sp, #464]
    ldp     q24, q25, [sp, #496]
    ldp     q26, q27, [sp, #528]
    ldp     q28, q29, [sp, #560]
    ldp     q30, q31, [sp, #592]
    add     sp, sp, #(112 + 512)
    ldp     x29, x30, [sp], #16
.endm

    .text

// A module's own thread-local. Argument: Aleks2TlsDescArg {module, offset}.
// Fast path: this thread's table already has the module's block.
    .global aleks2_tlsdesc_dynamic
    .type   aleks2_tlsdesc_dynamic, %function
    .p2align 4
aleks2_tlsdesc_dynamic:
    hint    #34                         // bti c: a module may be built with branch protection
    stp     x1, x2, [sp, #-32]!
    stp     x3, x4, [sp, #16]
    ldr     x0, [x0, #8]                // the argument
    host_tp x1
    add     x1, x1, #:tprel_hi12:aleks2_tls_dtv, lsl #12
    add     x1, x1, #:tprel_lo12_nc:aleks2_tls_dtv
    ldr     x1, [x1]                    // this thread's table, null before its first access
    cbz     x1, 1f
    ldp     x2, x3, [x0]                // module, offset
    ldr     x4, [x1]                    // capacity
    cmp     x2, x4
    b.hs    1f
    ldr     x4, [x1, x2, lsl #3]        // the block
    cbz     x4, 1f
    add     x0, x4, x3
2:  mrs     x1, tpidr_el0               // the module's thread pointer, whatever its value
    sub     x0, x0, x1
    ldp     x3, x4, [sp, #16]
    ldp     x1, x2, [sp], #32
    ret
1:  save_caller_saved
    bl      aleks2_tls_slow             // x0 = argument -> the variable's address
    restore_caller_saved
    b       2b
    .size   aleks2_tlsdesc_dynamic, . - aleks2_tlsdesc_dynamic

// A thread-local of the game. Argument: Aleks2TlsAccessorArg {accessor, addend}. Always slow: only
// std::call_once's two variables are reached this way.
    .global aleks2_tlsdesc_accessor
    .type   aleks2_tlsdesc_accessor, %function
    .p2align 4
aleks2_tlsdesc_accessor:
    hint    #34
    stp     x1, x2, [sp, #-32]!
    stp     x3, x4, [sp, #16]
    ldr     x0, [x0, #8]
    save_caller_saved
    bl      aleks2_tls_accessor_slow
    restore_caller_saved
    mrs     x1, tpidr_el0
    sub     x0, x0, x1
    ldp     x3, x4, [sp, #16]
    ldp     x1, x2, [sp], #32
    ret
    .size   aleks2_tlsdesc_accessor, . - aleks2_tlsdesc_accessor

#if defined(__linux__)
    .section .note.GNU-stack, "", %progbits
#endif

#endif
