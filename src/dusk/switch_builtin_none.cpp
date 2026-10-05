// aleks2 (H37): the built-in mod tables of a Switch build with no mod compiled into it
// (ALEKS2_NATIVE_MOD_LOADER without ALEKS2_STATIC_MODS). StaticMods.cmake generates them
// otherwise. Every native mod then comes from the card, through switch_native_loader.cpp.
#include "dusk/switch_builtin_mods.hpp"

namespace dusk::mods::loader::detail {

const BuiltinMod kBuiltinMods[] = {
    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
};

const BuiltinSymbol kBuiltinSymbols[] = {
    {nullptr, nullptr, false},
};

}  // namespace dusk::mods::loader::detail
