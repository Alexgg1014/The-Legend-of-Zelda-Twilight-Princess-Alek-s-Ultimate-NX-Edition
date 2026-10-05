/* aleks2 (H37-c2): see elf_image_runtime.h. */
#include "elf_image_runtime.h"

#include <math.h>
#include <unwind.h>

#if defined(__aarch64__)

/* libgcc's own prototypes (gcc/libgcc/soft-fp, libgcc2.c). On AArch64 long double is IEEE
 * binary128, libgcc's TFmode, so these are the declarations the compiler itself calls. */
int __eqtf2(long double, long double);
int __netf2(long double, long double);
int __gttf2(long double, long double);
int __unordtf2(long double, long double);
double __trunctfdf2(long double);
unsigned __int128 __udivti3(unsigned __int128, unsigned __int128);

const struct Aleks2ImageRuntimeEntry aleks2_image_runtime[] = {
    /* libgcc_eh: the unwinder the mod's frames are registered with. */
    {"_Unwind_Resume", (const void*)&_Unwind_Resume, "libgcc_eh"},
    /* libgcc: binary128 comparisons and conversion, 128-bit division. */
    {"__eqtf2", (const void*)&__eqtf2, "libgcc"},
    {"__gttf2", (const void*)&__gttf2, "libgcc"},
    {"__netf2", (const void*)&__netf2, "libgcc"},
    {"__trunctfdf2", (const void*)&__trunctfdf2, "libgcc"},
    {"__udivti3", (const void*)&__udivti3, "libgcc"},
    {"__unordtf2", (const void*)&__unordtf2, "libgcc"},
    /* libm: the library functions, not the fsqrt the compiler inlines for its own calls. */
    {"sqrt", (const void*)&sqrt, "libm"},
    {"sqrtf", (const void*)&sqrtf, "libm"},
    /* H38: hidden like sqrt; Twilit Essentials 2.5.0 (switch-arm64) imports both. */
    {"fmod", (const void*)&fmod, "libm"},
    {"fmodf", (const void*)&fmodf, "libm"},
};
const size_t aleks2_image_runtime_count = sizeof aleks2_image_runtime / sizeof aleks2_image_runtime[0];

#else

/* The host tests of the platform-neutral loader: they bring their own game image. */
const struct Aleks2ImageRuntimeEntry aleks2_image_runtime[] = {{0, 0, 0}};
const size_t aleks2_image_runtime_count = 0;

#endif
