// H37-c2: a mod library for elf_loader_image_test.cpp, built by test-elf-loader.sh like a
// linux-aarch64 package (a shared object against glibc/libstdc++), loaded by the loader into a
// static image whose libgcc_eh is hidden -- the console's situation.
//
// Every function here makes the unwinder go through a frame of this module:
//  * a cleanup with no handler (Guard's destructor) is a landing pad that ends in a call to
//    _Unwind_Resume, the import H37-c1 could not resolve;
//  * exceptions cross the boundary both ways, and one stays inside the module.
#include <stdexcept>

extern "C" void game_throw(int value);  // the image's: throws an int

namespace {

struct Guard {
    int* counter;
    ~Guard() { ++*counter; }
};

int g_constructed = 0;
int g_cleanups = 0;

struct StaticInit {
    StaticInit() { g_constructed += 42; }
};
StaticInit s_init;  // a constructor the loader must run, once

}  // namespace

extern "C" int mod_constructed() {
    return g_constructed;
}

extern "C" int mod_cleanups() {
    return g_cleanups;
}

// The image throws through this frame: the cleanup runs here, then _Unwind_Resume hands the
// exception back to the image's unwinder, which finds the image's handler.
extern "C" void mod_pass_through(int value) {
    Guard guard{&g_cleanups};
    game_throw(value);
}

// Thrown and caught inside the module, with a cleanup on the way.
extern "C" int mod_catch(int value) {
    try {
        Guard guard{&g_cleanups};
        if (value > 0) {
            throw std::runtime_error("inside the module");
        }
    } catch (const std::runtime_error&) {
        return value + 1;
    }
    return -1;
}

// Thrown here, caught by the image, through two module frames with cleanups.
static void __attribute__((noinline)) mod_deeper(int value) {
    Guard guard{&g_cleanups};
    throw value;
}

extern "C" void mod_throw(int value) {
    Guard guard{&g_cleanups};
    mod_deeper(value);
}

// What the loader requires every mod library to export (elf::kModExports); unused here.
extern "C" {
const void* mod_meta = nullptr;
void* mod_ctx = nullptr;
int mod_initialize(void*) { return 0; }
int mod_update(void*) { return 0; }
int mod_shutdown(void*) { return 0; }
}
