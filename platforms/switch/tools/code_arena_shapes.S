// DM2-a: function shapes for code_arena_funchook_test.c (aarch64). Each returns 3x + k for x >= 1.
// They decide between funchook's 12-byte patch (adrp + add + br) and the 8-byte one (adrp + br).

    .text

// Two instructions each, back to back with no padding: a 12-byte patch would overwrite the next
// function's first instruction. -> 8 bytes. k = 0.
    .p2align 4
    .global short_0, short_1, short_2
    .type short_0, %function
short_0:
    add w0, w0, w0, lsl #1
    ret
    .type short_1, %function
short_1:
    add w0, w0, w0, lsl #1
    ret
    .type short_2, %function
short_2:
    add w0, w0, w0, lsl #1
    ret
    nop

// A loop whose head is the third instruction (+8). -> 8 bytes. k = 200.
    .p2align 4
    .global loop8_0
    .type loop8_0, %function
loop8_0:
    mov w1, #200
    mov w2, w0
1:  add w1, w1, #3
    subs w2, w2, #1
    b.gt 1b
    mov w0, w1
    ret

// The same with the branch back 260 bytes in: past the 64 bytes funchook itself checks. k = 201.
    .p2align 4
    .global loopfar_0
    .type loopfar_0, %function
loopfar_0:
    mov w1, #201
    mov w2, w0
1:  add w1, w1, #3
    .rept 60
    nop
    .endr
    subs w2, w2, #1
    b.gt 1b
    mov w0, w1
    ret

// adrp as the third instruction, its add outside the patch: relocated as a literal load. -> 12. k = 300.
    .p2align 4
    .global adrp_0
    .type adrp_0, %function
adrp_0:
    mov w1, w0
    mov w2, #300
    adrp x3, three
    add x3, x3, :lo12:three
    ldr w4, [x3]
    madd w0, w1, w4, w2
    ret

// cbz as the second instruction: relocated (inverted, then a literal branch). -> 12. k = 400.
    .p2align 4
    .global cbz_0
    .type cbz_0, %function
cbz_0:
    mov w1, #400
    cbz w0, 2f
    add w1, w1, w0, lsl #1
    add w1, w1, w0
2:  mov w0, w1
    ret

// No branch back at all, but upstream funchook's check reads the word after the patch (the orn)
// with the id of the later b.ne: imm19 of that word is -2, "a jump to +4". Fixed: -> 12. k = 600.
    .p2align 4
    .global fpos_0
    .type fpos_0, %function
fpos_0:
    mov w1, #600
    mov w2, w0
    add w1, w1, w2
    orn x3, x30, xzr, ror #63
    add w1, w1, w2, lsl #1
    cmp w2, #0
    b.ne 1f
    nop
1:  mov w0, w1
    ret

// A loop whose head is the second instruction (+4): no patch can be made. -> refused. k = 500.
    .p2align 4
    .global bad4_0
    .type bad4_0, %function
bad4_0:
    mov w1, #500
1:  add w1, w1, #3
    subs w0, w0, #1
    b.gt 1b
    mov w0, w1
    ret

// The same with the branch back 260 bytes in. -> refused. k = 501.
    .p2align 4
    .global badfar4_0
    .type badfar4_0, %function
badfar4_0:
    mov w1, #501
1:  add w1, w1, #3
    .rept 60
    nop
    .endr
    subs w0, w0, #1
    b.gt 1b
    mov w0, w1
    ret

// DM2-e: functions shorter than the patch. A lone ret and zero padding to 16 bytes, like the game's
// dMenu_Ring_c::_delete(): the 12-byte patch covers the padding. Returns x.
    .p2align 4
    .global ret1_0
    .type ret1_0, %function
ret1_0:
    ret
    .word 0, 0, 0

// A lone ret with another function right after it: refused, the neighbour untouched. ret1_tight
// returns x, ret1_next 3x + 700.
    .p2align 4
    .global ret1_tight, ret1_next
    .type ret1_tight, %function
ret1_tight:
    ret
    .type ret1_next, %function
ret1_next:
    add w0, w0, w0, lsl #1
    add w0, w0, #700
    ret

    .data
    .p2align 2
three:
    .word 3
    .section .note.GNU-stack,"",%progbits
