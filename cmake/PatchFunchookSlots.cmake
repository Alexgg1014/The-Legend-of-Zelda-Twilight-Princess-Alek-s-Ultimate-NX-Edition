# aleks2 (DM2-a): several trampolines to a page. Included by PatchFunchook.cmake, after its own
# patches (it uses its _aleks2_funchook_site); idempotent, each file checks for its marker.
#
# funchook's arm64 patch, `adrp x9, page; br x9`, can only land on a page boundary, so each hook
# cost a whole page and the code arenas (eight CodeMemory objects of 64 pages; the kernel has about
# ten for the whole system) held 512 hooks. On __SWITCH__ the patch becomes, wherever the function
# allows it,
#
#     adrp x9, slot_page ; add x9, x9, #slot_offset ; br x9          (12 bytes, "wide")
#
# which reaches any 256-byte slot of an arena page (platforms/switch/source/code_arena.h). Where
# three instructions would overwrite more than the function -- a flow-ending instruction in its
# first two -- or something branches back into them, the old two-instruction patch is kept
# ("narrow") and the slot is asked for at a page boundary. Measured on the game's ELF: 279 of
# 85 728 hookable functions need the narrow patch; with it, nothing that hooked before stops.
#
# Both widths are checked for branches back into the patched instructions over up to 4 KiB of the
# function, the patched ones included (funchook's own check covers 64 bytes after them, with the
# disassembler): every such branch in the game is within 1 KiB of the function's start.
#
# How the width travels: make_trampoline picks it and leaves it in ip_displacement_t (a dummy int on
# arm64) for funchook_page_alloc (funchook_arena_glue.c), which asks the arena for a page-boundary
# slot or not; the entry records patch_size; install and uninstall write that many bytes;
# funchook_switch_fix_entry encodes it. JUMP32_SIZE is the larger width (3), for the entry's arrays
# and the trampoline's room. Other platforms compile upstream's lines.

file(READ "${SOURCE_DIR}/src/funchook_arm64.h" _content)
if (NOT _content MATCHES "aleks2 DM2-a")
    _aleks2_funchook_site(jump32_size
        [=[#define JUMP32_SIZE 2]=]
        [=[#ifdef __SWITCH__
/* aleks2 DM2-a: the wide patch, adrp + add + br (the narrow one, adrp + br, is 2) */
#define JUMP32_SIZE 3
#else
#define JUMP32_SIZE 2
#endif]=])
    _aleks2_funchook_site(literal_pool
        [=[#define LITERAL_POOL_OFFSET (3 * JUMP32_SIZE + 2)]=]
        [=[#ifdef __SWITCH__
/* aleks2 DM2-a: at most 3 instructions per relocated one, plus ldr + br back: 11. Even, so the
 * literal pool is 8-byte aligned in an entry. */
#define LITERAL_POOL_OFFSET (3 * JUMP32_SIZE + 3)
#else
#define LITERAL_POOL_OFFSET (3 * JUMP32_SIZE + 2)
#endif]=])
    _aleks2_funchook_site(patch_size
        [=[    uint32_t new_code[JUMP32_SIZE];
} funchook_entry_t;]=]
        [=[    uint32_t new_code[JUMP32_SIZE];
#ifdef __SWITCH__
    uint32_t patch_size; /* aleks2 DM2-a: bytes written over the target, 8 or 12 */
#endif
} funchook_entry_t;]=])
    file(WRITE "${SOURCE_DIR}/src/funchook_arm64.h" "${_content}")
endif ()

file(READ "${SOURCE_DIR}/src/funchook_arm64.c" _content)
if (NOT _content MATCHES "aleks2 DM2-a")
    _aleks2_funchook_site(make_signature
        [=[int funchook_make_trampoline(funchook_t *funchook, ip_displacement_t *disp, const insn_t *func, insn_t *trampoline, size_t *trampoline_size)
{]=]
        [=[#ifdef __SWITCH__
/* aleks2 DM2-a: one width per call; funchook_make_trampoline (further down) picks it. */
#define PATCH_INSNS patch_insns
static int aleks2_ends_flow(uint32_t ins)
{
    return (ins & 0xFC000000) == 0x14000000      /* b */
        || (ins & 0xFFFFFC1F) == 0xD65F0000      /* ret */
        || (ins & 0xFFFFFC1F) == 0xD61F0000      /* br */
        || (ins & 0xFFE0001F) == 0xD4200000      /* brk */
        || (ins & 0xFFE0001F) == 0xD4400000      /* hlt */
        || (ins & 0xFFFF0000) == 0x00000000;     /* udf, or zero padding */
}
static int make_trampoline_n(funchook_t *funchook, ip_displacement_t *disp, const insn_t *func, insn_t *trampoline, size_t *trampoline_size, const int patch_insns)
#else
#define PATCH_INSNS JUMP32_SIZE
int funchook_make_trampoline(funchook_t *funchook, ip_displacement_t *disp, const insn_t *func, insn_t *trampoline, size_t *trampoline_size)
#endif
{]=])
    _aleks2_funchook_site(ends_flow
        [=[        avail_regs &= ~info.regs;]=]
        [=[        avail_regs &= ~info.regs;
#ifdef __SWITCH__
        /* Wide only: the third instruction would belong to whatever follows the function. */
        if (patch_insns == JUMP32_SIZE && ctx.src - func < patch_insns - 1 && aleks2_ends_flow(ins)) {
            funchook_set_error_message(funchook, "the function ends within a 12-byte patch");
            rv = FUNCHOOK_ERROR_TOO_SHORT_INSTRUCTIONS;
            goto cleanup;
        }
#endif]=])
    _aleks2_funchook_site(copied_enough
        [=[        if (ctx.src - func >= JUMP32_SIZE) {]=]
        [=[        if (ctx.src - func >= PATCH_INSNS) {]=])
    _aleks2_funchook_site(back_jump
        [=[                if (func < target && target < func + JUMP32_SIZE) {]=]
        [=[                if (func < target && target < func + PATCH_INSNS) {]=])
    _aleks2_funchook_site(too_short
        [=[    if (ctx.src - func < JUMP32_SIZE) {]=]
        [=[    if (ctx.src - func < PATCH_INSNS) {]=])
    _aleks2_funchook_site(choose_width
        [=[int funchook_fix_code(funchook_t *funchook, funchook_entry_t *entry, const ip_displacement_t *disp)]=]
        [=[#ifdef __SWITCH__
/* The platform: the end of the readable code region holding `func`. */
uintptr_t funchook_switch_scan_end(const void *func);

/* aleks2 DM2-a: does anything in the 4 KiB from `func` -- the patched instructions included --
 * branch into (func, func + patch_insns)? Such a branch would land inside the patch. */
static int aleks2_branches_into(const insn_t *func, int patch_insns)
{
    uintptr_t end = (uintptr_t)func + 4096;
    const uintptr_t mapped = funchook_switch_scan_end(func);
    if (mapped < end) {
        end = mapped;
    }
    const uintptr_t first = (uintptr_t)func + 4, last = (uintptr_t)func + 4 * (uintptr_t)patch_insns;
    for (const insn_t *p = func; (uintptr_t)(p + 1) <= end; p++) {
        const uint32_t ins = *p;
        int64_t off;
        if ((ins & 0x7C000000) == 0x14000000) {                                            /* b, bl */
            off = IMM26_OFFSET(ins);
        } else if ((ins & 0xFF000010) == 0x54000000 || (ins & 0x7E000000) == 0x34000000) { /* b.cond, cbz */
            off = IMM19_OFFSET(ins);
        } else if ((ins & 0x7E000000) == 0x36000000) {                                     /* tbz, tbnz */
            off = IMM14_OFFSET(ins);
        } else {
            continue;
        }
        const uintptr_t target = (uintptr_t)p + (uintptr_t)off;
        if (target >= first && target < last) {
            return 1;
        }
    }
    return 0;
}

/* Wide where the function allows it, else narrow. The width, in instructions, is left in
 * disp->dummy for funchook_page_alloc: the narrow patch needs a page-boundary slot. */
int funchook_make_trampoline(funchook_t *funchook, ip_displacement_t *disp, const insn_t *func, insn_t *trampoline, size_t *trampoline_size)
{
    if (!aleks2_branches_into(func, JUMP32_SIZE)) {
        const int rv = make_trampoline_n(funchook, disp, func, trampoline, trampoline_size, JUMP32_SIZE);
        if (rv == 0) {
            disp->dummy = JUMP32_SIZE;
            return 0;
        }
        funchook_log(funchook, "  12-byte patch refused (%d); trying the 8-byte one\n", rv);
    }
    if (aleks2_branches_into(func, 2)) {
        funchook_set_error_message(funchook, "something branches back into the function's first two instructions");
        return FUNCHOOK_ERROR_FOUND_BACK_JUMP;
    }
    const int rv = make_trampoline_n(funchook, disp, func, trampoline, trampoline_size, 2);
    if (rv == 0) {
        disp->dummy = 2;
    }
    return rv;
}
#endif

int funchook_fix_code(funchook_t *funchook, funchook_entry_t *entry, const ip_displacement_t *disp)]=])
    file(WRITE "${SOURCE_DIR}/src/funchook_arm64.c" "${_content}")
endif ()

# Upstream's check for branches back into the patch decodes *ctx.src -- the first instruction after
# the patch, never advanced in that loop -- with the id of each instruction it scans: false
# positives (two Link hooks, of Twilit and of Hide & Seek, refused in LOADERONLY-c1) and no real
# check. On __SWITCH__ each scanned instruction is decoded at its own address; aleks2_branches_into
# checks 4 KiB besides. Its own marker, so a tree patched before this site still gets it.
file(READ "${SOURCE_DIR}/src/funchook_arm64.c" _content)
if (NOT _content MATCHES "aleks2 DM2-a: decode at")
    _aleks2_funchook_site(back_jump_decode
        [=[                const insn_t *target = (const insn_t *)target_addr((size_t)ctx.src, *ctx.src, info.insn_id);]=]
        [=[#ifdef __SWITCH__
                const insn_t *at = (const insn_t *)funchook_insn_address(insn); /* aleks2 DM2-a: decode at its address */
                const insn_t *target = (const insn_t *)target_addr((size_t)at, *at, info.insn_id);
#else
                const insn_t *target = (const insn_t *)target_addr((size_t)ctx.src, *ctx.src, info.insn_id);
#endif]=])
    file(WRITE "${SOURCE_DIR}/src/funchook_arm64.c" "${_content}")
endif ()

file(READ "${SOURCE_DIR}/src/funchook.c" _content)
if (NOT _content MATCHES "aleks2 DM2-a")
    _aleks2_funchook_site(old_code
        [=[    memcpy(entry->old_code, func, JUMP32_BYTE_SIZE);]=]
        [=[#ifdef __SWITCH__
    /* aleks2 DM2-a: 8 or 12 bytes, as make_trampoline chose */
    entry->patch_size = (uint32_t)disp.dummy * (uint32_t)sizeof(insn_t);
    memcpy(entry->old_code, func, entry->patch_size);
#else
    memcpy(entry->old_code, func, JUMP32_BYTE_SIZE);
#endif]=])
    _aleks2_funchook_site(install_size
        [=[                                            entry->new_code, JUMP32_BYTE_SIZE);]=]
        [=[                                            entry->new_code, FUNCHOOK_PATCH_BYTES(entry));]=])
    _aleks2_funchook_site(uninstall_size
        [=[                                            entry->old_code, JUMP32_BYTE_SIZE);]=]
        [=[                                            entry->old_code, FUNCHOOK_PATCH_BYTES(entry));]=])
    string(FIND "${_content}" "static int get_page(" _first)
    if (_first EQUAL -1)
        message(FATAL_ERROR "Funchook get_page changed")
    endif ()
    string(SUBSTRING "${_content}" 0 ${_first} _head)
    string(SUBSTRING "${_content}" ${_first} -1 _tail)
    set(_content "${_head}#ifdef __SWITCH__
#define FUNCHOOK_PATCH_BYTES(entry) ((entry)->patch_size) /* aleks2 DM2-a */
#else
#define FUNCHOOK_PATCH_BYTES(entry) JUMP32_BYTE_SIZE
#endif
${_tail}")
    file(WRITE "${SOURCE_DIR}/src/funchook.c" "${_content}")
endif ()

# aleks2 (DM2-e): functions shorter than the patch. A function that ends -- ret, b, br, brk, hlt -- before
# the 8- or 12-byte patch does could not be hooked ("Too short instructions"), although the bytes up to
# the patch's end are alignment padding nothing executes: Twilit Essentials 2.8.4 hooks
# dMenu_Ring_c::_delete(), a lone `ret` with 12 zero bytes before the next function (functions are
# 16-byte aligned). Upstream already counts B + UDF as two instructions; on __SWITCH__ this
# generalizes it: when the instructions copied so far end the flow and every word from there to the
# patch's end is zero, the patch covers those words. Zero only, never NOP: no function starts with
# 0x00000000 (udf), so the patch cannot reach into the next one. The trampoline's jump back lands on
# the padding and is never taken (the copied instruction leaves first). Its own marker.
file(READ "${SOURCE_DIR}/src/funchook_arm64.c" _content)
if (NOT _content MATCHES "aleks2 DM2-e")
    _aleks2_funchook_site(short_helpers
        [=[static int make_trampoline_n(funchook_t *funchook, ip_displacement_t *disp, const insn_t *func, insn_t *trampoline, size_t *trampoline_size, const int patch_insns)]=]
        [=[/* aleks2 DM2-e: a flow-ending instruction proper (not zero padding), and words that are all zero. */
static int aleks2_real_end(uint32_t ins)
{
    return aleks2_ends_flow(ins) && (ins & 0xFFFF0000) != 0x00000000;
}
static int aleks2_zero_to(const insn_t *from, const insn_t *to)
{
    for (const insn_t *p = from; p < to; p++) {
        if (*p != 0) {
            return 0;
        }
    }
    return 1;
}
static int make_trampoline_n(funchook_t *funchook, ip_displacement_t *disp, const insn_t *func, insn_t *trampoline, size_t *trampoline_size, const int patch_insns)]=])
    _aleks2_funchook_site(short_wide
        [=[        if (patch_insns == JUMP32_SIZE && ctx.src - func < patch_insns - 1 && aleks2_ends_flow(ins)) {]=]
        [=[        if (patch_insns == JUMP32_SIZE && ctx.src - func < patch_insns - 1 && aleks2_ends_flow(ins)
            && !(aleks2_real_end(ins) && aleks2_zero_to(ctx.src + 1, func + patch_insns))) { /* aleks2 DM2-e */]=])
    _aleks2_funchook_site(short_padding
        [=[            ctx.src = func + 2;
        }
        if (ctx.src - func >= PATCH_INSNS) {]=]
        [=[            ctx.src = func + 2;
        }
#ifdef __SWITCH__
        /* aleks2 DM2-e: the function has ended and only zero padding is left to the patch's end. */
        if (ctx.src - func < PATCH_INSNS && aleks2_real_end(ins) && aleks2_zero_to(ctx.src, func + PATCH_INSNS)) {
            ctx.src = func + PATCH_INSNS;
        }
#endif
        if (ctx.src - func >= PATCH_INSNS) {]=])
    file(WRITE "${SOURCE_DIR}/src/funchook_arm64.c" "${_content}")
endif ()

# aleks2 (DM2-e): the same check for the 8-byte patch. funchook does not stop disassembling at a ret: a
# one-instruction function with another function right after it (no padding) was hooked with the 8-byte
# patch, which then overwrote the neighbour's first instruction (code_arena_shapes.S: ret1_tight). A
# flow-ending instruction before the patch's last one is refused at either width unless only zero padding
# follows it. Its own marker, for trees the block above already patched.
file(READ "${SOURCE_DIR}/src/funchook_arm64.c" _content)
if (NOT _content MATCHES "aleks2 DM2-e: any width")
    _aleks2_funchook_site(short_any_width
        [=[        if (patch_insns == JUMP32_SIZE && ctx.src - func < patch_insns - 1 && aleks2_ends_flow(ins)
            && !(aleks2_real_end(ins) && aleks2_zero_to(ctx.src + 1, func + patch_insns))) { /* aleks2 DM2-e */
            funchook_set_error_message(funchook, "the function ends within a 12-byte patch");]=]
        [=[        if (ctx.src - func < patch_insns - 1 && aleks2_ends_flow(ins)
            && !(aleks2_real_end(ins) && aleks2_zero_to(ctx.src + 1, func + patch_insns))) { /* aleks2 DM2-e: any width */
            funchook_set_error_message(funchook, "the function ends within the patch, and what follows is not zero padding");]=])
    file(WRITE "${SOURCE_DIR}/src/funchook_arm64.c" "${_content}")
endif ()
