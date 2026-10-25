#pragma once

// aleks2 (1.0.2): symgen stores demangled names with their ABI tags ("dusk::ui::midna_icon_source[abi:cxx11]"), and
// a mod that names the function without the tag (Twilight HD HUD) found nothing. When the exact name is not in the
// manifest, manifest::resolve compares names with every [abi:...] tag removed from both sides: a single function is a
// match, two or more are ambiguous. check_symbol_manifest.lookup() does the same for the gates.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace dusk::mods::abi_tags {

inline std::string strip(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size();) {
        if (name.compare(i, 5, "[abi:") == 0) {
            if (const size_t close = name.find(']', i); close != std::string_view::npos) {
                i = close + 1;
                continue;
            }
        }
        out.push_back(name[i++]);
    }
    return out;
}

constexpr int64_t kNone = -1;
constexpr int64_t kAmbiguous = -2;

// The index of the one name among `count` that equals `name` once ABI tags are ignored, kNone, or kAmbiguous when
// names at different addresses match. nameAt(i) returns the i-th name (nullptr to skip it), addressAt(i) its address.
// Linear: it only runs after an exact lookup failed.
template <class NameAt, class AddressAt>
int64_t find_ignoring_tags(std::string_view name, uint32_t count, NameAt nameAt, AddressAt addressAt) {
    const std::string key = strip(name);
    const bool tagged = key.size() != name.size();
    int64_t found = kNone;
    for (uint32_t i = 0; i < count; ++i) {
        const char* candidate = nameAt(i);
        if (candidate == nullptr) {
            continue;
        }
        bool match;
        if (std::strstr(candidate, "[abi:") != nullptr) {
            match = strip(candidate) == key;
        } else {
            match = tagged && key == candidate;  // an untagged request already failed the exact lookup
        }
        if (!match) {
            continue;
        }
        if (found < 0) {
            found = i;
        } else if (addressAt(static_cast<uint32_t>(found)) != addressAt(i)) {
            return kAmbiguous;
        }
    }
    return found;
}

}  // namespace dusk::mods::abi_tags
