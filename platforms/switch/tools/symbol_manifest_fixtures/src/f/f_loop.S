// Fixtures for test-symbol-manifest.sh (H37-c2): a function whose loop branches back to its
// second instruction, inside the two funchook overwrites -- funchook refuses to hook it; and a
// one-instruction function with the next function right after it, no padding -- the patch
// would overwrite the neighbour.
    .text
    .globl  loop_target
    .type   loop_target, %function
loop_target:
    mov     x1, x0
1:  subs    x1, x1, #1
    b.ne    1b
    ret
    .size   loop_target, . - loop_target

    .globl  short_target
    .type   short_target, %function
short_target:
    b       adjacent
    .size   short_target, . - short_target
    .type   adjacent, %function
adjacent:
    add     x0, x0, #1
    ret
    .size   adjacent, . - adjacent
