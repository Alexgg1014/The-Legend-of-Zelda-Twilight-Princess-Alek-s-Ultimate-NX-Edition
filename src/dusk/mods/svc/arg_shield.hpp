#pragma once

// aleks2 (V1-rc9): what a hook callback reads as its first argument is a copy in the dispatcher's own frame.
//
// The SDK's trampoline (compiled into the mod that installed the detour) passes `args` as an array of pointers to its
// own copies of the arguments: args[0] is the address of the first one. A callback that takes `args` itself for that
// array's element type -- `*static_cast<T**>(args)` instead of `mods::hook::arg<T*>(args, 0)` -- gets the address
// of the trampoline's stack slot, not the object, and writes through it into the trampoline's frame: Twilight Compass
// 1.0.2 on dMeterMap_c::keyCheck/draw wrote 0 and 7 at +0x2F/+0x30 of the slot, which on Switch is a saved register
// of ctrlShowMap (this = 0x0000000700748400) or of the draw-list loop. On PC the same bytes were harmless.
//
// So for the callbacks, args[0] points at a copy here, 8 bytes of the first argument and padding after it: a write
// meant for "the object" lands in the padding. Afterwards args[0] is put back and the bytes of the copy a callback
// changed are written to the real slot -- only those, so an argument smaller than 8 bytes never gets its neighbours
// overwritten. A correct callback reads and writes the same value it would have; only arguments after the first
// keep their own slots (the array's length is the trampoline's, unknown here).
#include <cstddef>
#include <cstring>

namespace dusk::mods::svc {

class ArgShield {
public:
    static constexpr std::size_t kPadding = 512;

    ArgShield(void* args, bool enabled) noexcept {
        if (!enabled || args == nullptr) {
            return;
        }
        mPtrs = static_cast<void**>(args);
        mSlot = static_cast<unsigned char*>(mPtrs[0]);
        if (mSlot == nullptr) {
            mPtrs = nullptr;
            return;
        }
        std::memcpy(mCopy, mSlot, 8);
        std::memcpy(mBefore, mSlot, 8);
        mPtrs[0] = mCopy;
    }

    ~ArgShield() {
        if (mPtrs == nullptr) {
            return;
        }
        mPtrs[0] = mSlot;
        for (int i = 0; i < 8; ++i) {
            if (mCopy[i] != mBefore[i]) {
                mSlot[i] = mCopy[i];
            }
        }
    }

    ArgShield(const ArgShield&) = delete;
    ArgShield& operator=(const ArgShield&) = delete;

private:
    void** mPtrs = nullptr;
    unsigned char* mSlot = nullptr;
    unsigned char mBefore[8] = {};
    alignas(16) unsigned char mCopy[8 + kPadding];  // not cleared: nothing reads the padding
};

}  // namespace dusk::mods::svc
