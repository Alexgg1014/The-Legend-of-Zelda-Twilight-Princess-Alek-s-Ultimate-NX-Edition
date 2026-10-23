#pragma once
#include <string>
#include <vector>
namespace dusk::mods {
struct ShimMetadata {
    std::string id;
    std::string version;
};
struct ShimMod {
    ShimMetadata metadata;
};
class ModLoader {
public:
    static ModLoader& instance();
    const std::vector<ShimMod>& mods() const;
};
}  // namespace dusk::mods
