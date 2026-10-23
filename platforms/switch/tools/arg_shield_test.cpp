// V1-rc9: src/dusk/mods/svc/arg_shield.hpp against Twilight Compass 1.0.2's write pattern (test-arg-shield.sh).
//
// A trampoline frame is laid out as the SDK's (mods/svc/hook.hpp) makes it: the arguments' copies, the array of
// pointers to them passed as `args`, and around them what the caller's frame holds (here a saved register 0x30 past
// the first slot, as ctrlShowMap's x19 was). The "callbacks" are what mods do with args.
#include "dusk/mods/svc/arg_shield.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

using dusk::mods::svc::ArgShield;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

struct MeterMap {  // dMeterMap_c's two bytes Compass writes (setDispPosOutsideFlg_SE_On)
    unsigned char pad[0x2F];
    unsigned char mapIsInside;
    unsigned char field_0x30;
};

// The trampoline's frame: the first argument's slot, then the caller's saved register 0x30 bytes on.
struct Frame {
    alignas(16) unsigned char slot0[0x30];  // the first argument (8 bytes) and what follows it in the frame
    std::uint64_t savedX19;                  // at slot0 + 0x30
    std::uint8_t arg1;                       // a small second argument
    void* ptrs[2];
};

// Compass 1.0.2: auto* map = *static_cast<dMeterMap_c**>(args); map->setDispPosOutsideFlg_SE_On();
void compass_pre(void* args) {
    auto* map = *static_cast<MeterMap**>(args);
    if (map != nullptr) {
        map->mapIsInside = 0;
        map->field_0x30 = 7;
    }
}

// A correct mod: mods::hook::arg<MeterMap*>(args, 0) -- through the pointer.
MeterMap* correct_read(void* args) {
    return *static_cast<MeterMap**>(static_cast<void**>(args)[0]);
}

}  // namespace

int main() {
    MeterMap realMap{};
    realMap.mapIsInside = 1;

    // 1. Without the shield, Compass's write lands on the saved register (the rc8 crash).
    {
        Frame f{};
        MeterMap* self = &realMap;
        std::memcpy(f.slot0, &self, 8);
        f.savedX19 = 0x0000007d9b748400ull;
        f.ptrs[0] = f.slot0;
        f.ptrs[1] = &f.arg1;
        {
            const ArgShield off{f.ptrs, false};
            compass_pre(f.ptrs);
        }
        CHECK(f.savedX19 != 0x0000007d9b748400ull);  // clobbered, as on the console
    }
    // 2. With it, the saved register and the real object's slot are untouched; the write went to the padding.
    {
        Frame f{};
        MeterMap* self = &realMap;
        std::memcpy(f.slot0, &self, 8);
        f.savedX19 = 0x0000007d9b748400ull;
        f.ptrs[0] = f.slot0;
        f.ptrs[1] = &f.arg1;
        {
            const ArgShield on{f.ptrs, true};
            CHECK(f.ptrs[0] != f.slot0);
            CHECK(correct_read(f.ptrs) == &realMap);  // a correct read still gets the object
            compass_pre(f.ptrs);
            CHECK(correct_read(f.ptrs) == &realMap);  // and the copy of the pointer survived the write
        }
        CHECK(f.savedX19 == 0x0000007d9b748400ull);
        MeterMap* after = nullptr;
        std::memcpy(&after, f.slot0, 8);
        CHECK(after == &realMap);
        CHECK(f.ptrs[0] == f.slot0);  // put back for dispatch_post and the original
        CHECK(realMap.mapIsInside == 1);
    }
    // 3. A correct mod that changes the first argument: the change reaches the trampoline's slot.
    {
        Frame f{};
        MeterMap other{};
        MeterMap* self = &realMap;
        std::memcpy(f.slot0, &self, 8);
        f.ptrs[0] = f.slot0;
        {
            const ArgShield on{f.ptrs, true};
            *static_cast<MeterMap**>(f.ptrs[0]) = &other;
        }
        MeterMap* after = nullptr;
        std::memcpy(&after, f.slot0, 8);
        CHECK(after == &other);
    }
    // 4. A one-byte first argument changed by a correct mod: only that byte is written back; the 7 bytes after it
    //    (another local of the trampoline) are left as they are, even if they changed meanwhile.
    {
        alignas(8) unsigned char slot[16];
        for (int i = 0; i < 16; ++i) {
            slot[i] = static_cast<unsigned char>(0xA0 + i);
        }
        void* ptrs[1] = {slot};
        {
            const ArgShield on{ptrs, true};
            *static_cast<unsigned char*>(ptrs[0]) = 0x55;
            slot[3] = 0x33;  // the trampoline's neighbour changes during the callbacks (not through args)
        }
        CHECK(slot[0] == 0x55);
        CHECK(slot[3] == 0x33);
        for (int i = 1; i < 16; ++i) {
            if (i != 3) {
                CHECK(slot[i] == static_cast<unsigned char>(0xA0 + i));
            }
        }
    }
    // 5. No arguments (args == nullptr) and a null first pointer: nothing touched.
    {
        const ArgShield none{nullptr, true};
        void* ptrs[1] = {nullptr};
        {
            const ArgShield nullFirst{ptrs, true};
        }
        CHECK(ptrs[0] == nullptr);
    }
    if (g_failures == 0) {
        std::puts("arg shield: Compass's write through args lands in the copy's padding (saved register intact, the "
                  "rc8 crash without it), correct reads and writes reach the real slot, a 1-byte argument's "
                  "neighbours untouched: ok");
    }
    return g_failures == 0 ? 0 : 1;
}
