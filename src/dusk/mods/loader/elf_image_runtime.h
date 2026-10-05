#pragma once
/* aleks2 (H37-c2): toolchain runtime functions the game image contains but cannot export.
 *
 * The NRO links devkitA64's libgcc, libgcc_eh and newlib's libm statically, and some of their
 * functions are defined with hidden visibility there (libgcc's soft-float and 128-bit integer
 * helpers, libgcc_eh's unwinder, and the libm square roots in the H37-c1 image). The linker keeps
 * a hidden definition out of .dynsym whatever --dynamic-list says -- it becomes a local symbol --
 * so ImageSymbols (the image's own .dynsym) never finds them, and a mod that imports one cannot be
 * planned. H37-c1's audit: _Unwind_Resume (loader_lab, Cosmetics, Randomizer), __eqtf2, __gttf2,
 * __netf2, __trunctfdf2, __udivti3, __unordtf2 (Cosmetics, Randomizer), sqrt, sqrtf (Randomizer).
 *
 * The table below names them, one by one, with the address the linker itself resolved for each
 * reference: the image's own function, the one the game calls -- not a copy, not a wrapper.
 * That matters most for _Unwind_Resume: a mod's cleanup landing pads must hand the exception
 * back to the unwinder whose frame registry knows the mod's .eh_frame (__register_frame, in
 * switch_native_loader.cpp), which is this image's. A second libgcc_eh linked into a mod would
 * have an empty registry of its own.
 *
 * It is a closed list: nothing is exported wholesale and no local symbol is looked up by name at
 * run time. The addresses are ordinary pointers in .data.rel.ro, relocated by crt0 with the rest
 * of the image, so ASLR moves them with the image. check_symbol_manifest.py reads this same table
 * out of the linked ELF (symbol aleks2_image_runtime, its RELATIVE relocations) and checks every
 * entry against the ELF's own symbol table: the address must be the start of the function of that
 * name. A name added here therefore needs a function that really exists in the image.
 *
 * C, not C++: an address constant in a static initializer is constant initialization in C, so the
 * table is data in the file, never filled in by a constructor. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Aleks2ImageRuntimeEntry {
    const char* name;     /* the import name a mod uses */
    const void* address;  /* the image's own definition */
    const char* library;  /* where it comes from, for reports */
};

/* Layout read by platforms/switch/tools/check_symbol_manifest.py: three pointers per entry. */
extern const struct Aleks2ImageRuntimeEntry aleks2_image_runtime[];
extern const size_t aleks2_image_runtime_count;

#ifdef __cplusplus
}
#endif
