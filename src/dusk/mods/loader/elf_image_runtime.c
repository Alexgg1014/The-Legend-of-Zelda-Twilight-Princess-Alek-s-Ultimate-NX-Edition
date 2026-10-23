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
/* V1 (Dawnlight 5.0.0 "Hyrule Survivors", which does long double arithmetic): the rest of libgcc's binary128
 * family, so a mod that computes in long double resolves whatever helpers its compiler emitted. */
long double __addtf3(long double, long double);
long double __subtf3(long double, long double);
long double __multf3(long double, long double);
long double __divtf3(long double, long double);
long double __negtf2(long double);
long double __powitf2(long double, int);
int __lttf2(long double, long double);
int __letf2(long double, long double);
int __getf2(long double, long double);
long double __extendsftf2(float);
long double __extenddftf2(double);
float __trunctfsf2(long double);
int __fixtfsi(long double);
unsigned __fixunstfsi(long double);
long long __fixtfdi(long double);
unsigned long long __fixunstfdi(long double);
__int128 __fixtfti(long double);
unsigned __int128 __fixunstfti(long double);
long double __floatsitf(int);
long double __floatunsitf(unsigned);
long double __floatditf(long long);
long double __floatunditf(unsigned long long);
long double __floattitf(__int128);
long double __floatuntitf(unsigned __int128);

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
    /* V1: binary128 arithmetic and conversions (Dawnlight 5.0.0; the whole family, not only its six). */
    {"__addtf3", (const void*)&__addtf3, "libgcc"},
    {"__subtf3", (const void*)&__subtf3, "libgcc"},
    {"__multf3", (const void*)&__multf3, "libgcc"},
    {"__divtf3", (const void*)&__divtf3, "libgcc"},
    {"__negtf2", (const void*)&__negtf2, "libgcc"},
    {"__powitf2", (const void*)&__powitf2, "libgcc"},
    {"__lttf2", (const void*)&__lttf2, "libgcc"},
    {"__letf2", (const void*)&__letf2, "libgcc"},
    {"__getf2", (const void*)&__getf2, "libgcc"},
    {"__extendsftf2", (const void*)&__extendsftf2, "libgcc"},
    {"__extenddftf2", (const void*)&__extenddftf2, "libgcc"},
    {"__trunctfsf2", (const void*)&__trunctfsf2, "libgcc"},
    {"__fixtfsi", (const void*)&__fixtfsi, "libgcc"},
    {"__fixunstfsi", (const void*)&__fixunstfsi, "libgcc"},
    {"__fixtfdi", (const void*)&__fixtfdi, "libgcc"},
    {"__fixunstfdi", (const void*)&__fixunstfdi, "libgcc"},
    {"__fixtfti", (const void*)&__fixtfti, "libgcc"},
    {"__fixunstfti", (const void*)&__fixunstfti, "libgcc"},
    {"__floatsitf", (const void*)&__floatsitf, "libgcc"},
    {"__floatunsitf", (const void*)&__floatunsitf, "libgcc"},
    {"__floatditf", (const void*)&__floatditf, "libgcc"},
    {"__floatunditf", (const void*)&__floatunditf, "libgcc"},
    {"__floattitf", (const void*)&__floattitf, "libgcc"},
    {"__floatuntitf", (const void*)&__floatuntitf, "libgcc"},
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
