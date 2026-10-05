#pragma once
// aleks2 (H37): what a native mod may bind to when elf_loader maps it on Switch.
//
// A mod's imports resolve against one image: the game's NRO, with the newlib C library, the
// libstdc++ and the libgcc it was linked with. There is no other library to load beside a mod.
// This file decides, per import and per DT_NEEDED, whether that is acceptable, from what the
// audited packages actually ask for (docs/switch-h37-dynamic-loader.md, platforms/switch/tools/
// elf_audit.py):
//
//  * Packages built for Switch (lib/switch-arm64/mod.so, devkitA64 against newlib) need nothing
//    special: every import is a name the game image defines.
//  * Packages built for Linux (lib/linux-aarch64/mod.so) are the same architecture and ABI for
//    the C++ code itself, but were linked against glibc. They are accepted only in Linux
//    compatibility mode, with every difference handled explicitly:
//      - DT_NEEDED on the five Linux runtime libraries (libc, libm, libstdc++, libgcc_s, the
//        dynamic linker) is satisfied by the game image; anything else is refused.
//      - Symbol versions (GLIBC_2.17, GLIBCXX_3.4.x...) are dropped: the name must be one the
//        game defines, with the same meaning.
//      - A short list of glibc-only names gets a shim with the same semantics (shims()).
//      - Names whose ABI differs between glibc and newlib are refused by name (denied()):
//        pthread objects (glibc's sizes and layouts), stdio streams as data, the dynamic
//        linker's API, glibc internals.
//      - H37-c6: so are the C++ stream and locale families whose layout the libstdc++ headers
//        decide (std::ios_base, basic_ios, the fstream and stringstream classes; not std::locale alone): a
//        Linux module was compiled against another libstdc++ than the game's, so it needs a
//        switch-arm64 library (denied()).
//  * Either way, a few toolchain runtime functions are in the image but hidden from its exports;
//    they resolve through an explicit table of the image's own definitions (image_runtime()).
//
// Pure: the game's symbols come from a lookup the caller passes (the symbol manifest on
// Switch, a fake table in tests).
#include "elf_loader.hpp"

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace dusk::mods::elf {

enum class PackageAbi : uint8_t {
    Switch,  // lib/switch-arm64: built against newlib
    Linux,   // lib/linux-aarch64: built against glibc, loaded in compatibility mode
};

// One stand-in for a glibc-only name.
struct Shim {
    std::string_view name;
    uint64_t address;         // the stand-in, in the game image
    std::string_view reason;  // what it stands for, for reports
};

// The game image's symbols: address of an exported (allowlisted) name, or nullopt.
using GameLookup = std::function<std::optional<uint64_t>(std::string_view name)>;

// The shims this build provides (elf_runtime_shims.cpp), with their addresses.
std::vector<Shim> shims();

// H37-c2: toolchain runtime functions the game image links but cannot export -- hidden in
// libgcc, libgcc_eh and libm, so absent from .dynsym -- with the address of the image's own
// definition (elf_image_runtime.h). Consulted after the game's exports, for any package ABI.
std::vector<Shim> image_runtime();

// Why `name` is refused for a Linux-built module, or nullopt if it is not.
std::optional<std::string_view> denied(std::string_view name);

// The Linux runtime libraries the game image stands in for.
bool provided_needed(std::string_view soname);

Resolver make_resolver(PackageAbi abi, GameLookup game, std::vector<Shim> shimTable = shims(),
    std::vector<Shim> imageRuntime = image_runtime());
NeededPolicy make_needed_policy(PackageAbi abi);

}  // namespace dusk::mods::elf
