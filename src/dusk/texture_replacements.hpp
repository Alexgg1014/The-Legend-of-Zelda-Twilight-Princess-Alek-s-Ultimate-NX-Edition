#pragma once

#include <cstddef>
#include <cstdint>

namespace dusk::texture_replacements {

// Mod replacements are prioritized *over* user replacements (<data folder>/texture_replacements/)
inline constexpr int32_t kUserTextureReplacementPriority = -1'000'000;

void reload();
void shutdown();
// aleks2 (V1-rc12): registrations from the texture_replacements directory (0: no pack installed).
std::size_t directory_registrations();

}
