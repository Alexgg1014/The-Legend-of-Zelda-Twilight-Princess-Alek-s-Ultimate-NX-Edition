#pragma once

// aleks2 (H38-c6): the console findings of hardware_findings.inc, for the mod browser and the update check.
// Header-only and free of the game so a host test can compile it. h38_policy.py reads the same .inc for the gate.

#include <array>
#include <string_view>

namespace dusk::mods::hardware {

enum class Status {
    Unverified,
    Tested,
    Inactive,
    Incompatible,
};

struct Finding {
    std::string_view id;
    std::string_view version;
    std::string_view sha256;  // of the .dusk that was run; empty when not recorded
    Status status;
    std::string_view reason;
};

// aleks2 (V1-rc5): a mod this build ships its own Switch build of, and that build's version. A catalog version up to
// that one is refused with `note` (it would replace a Switch build with an older Linux one); a newer one installs
// and updates normally, through the linux-aarch64 compatibility layer, as any other catalog mod.
struct SwitchVariant {
    std::string_view id;
    std::string_view version;
    std::string_view note;
};

// a.b.c... compared numerically part by part; a non-digit ends a part (1.0.6-rc1 compares as 1.0.6).
constexpr int compare_versions(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        unsigned long x = 0, y = 0;
        while (i < a.size() && a[i] >= '0' && a[i] <= '9') {
            x = x * 10 + static_cast<unsigned long>(a[i++] - '0');
        }
        while (j < b.size() && b[j] >= '0' && b[j] <= '9') {
            y = y * 10 + static_cast<unsigned long>(b[j++] - '0');
        }
        if (x != y) {
            return x < y ? -1 : 1;
        }
        const bool moreA = i < a.size() && a[i] == '.';
        const bool moreB = j < b.size() && b[j] == '.';
        if (!moreA && !moreB) {
            break;
        }
        i += moreA ? 1 : a.size();
        j += moreB ? 1 : b.size();
    }
    return 0;
}

namespace detail {
using enum Status;
#define DUSK_MOD_HARDWARE(id, version, sha256, status, reason) Finding{id, version, sha256, status, reason},
#define DUSK_MOD_SWITCH_VARIANT(id, version, note)
inline constexpr Finding kFindings[] = {
#include "hardware_findings.inc"
};
#undef DUSK_MOD_HARDWARE
#undef DUSK_MOD_SWITCH_VARIANT
#define DUSK_MOD_HARDWARE(id, version, sha256, status, reason)
#define DUSK_MOD_SWITCH_VARIANT(id, version, note) SwitchVariant{id, version, note},
inline constexpr SwitchVariant kSwitchVariants[] = {
#include "hardware_findings.inc"
};
#undef DUSK_MOD_HARDWARE
#undef DUSK_MOD_SWITCH_VARIANT
}  // namespace detail

inline constexpr std::string_view kUnverifiedReason = "Not shown to work on the console for this build yet.";

struct Verdict {
    Status status = Status::Unverified;
    std::string_view reason = kUnverifiedReason;
    bool listed = false;  // the mod id has an entry (for this or another version)
};

/*
 * What the console showed for this exact package. An entry applies only to its id and version and, when both the
 * entry and the caller know the package's SHA-256, to those bytes: another version or other bytes are Unverified.
 * Pass an empty sha256 when the caller has no reliable hash (a catalog card only has the id and version).
 */
constexpr Verdict verdict(std::string_view id, std::string_view version, std::string_view sha256 = {}) {
    Verdict out;
    for (const auto& finding : detail::kFindings) {
        if (finding.id != id) {
            continue;
        }
        out.listed = true;
        if (finding.version != version) {
            continue;
        }
        if (!finding.sha256.empty() && !sha256.empty() && finding.sha256 != sha256) {
            continue;
        }
        out.status = finding.status;
        out.reason = finding.reason;
        return out;
    }
    return out;
}

/* The note for a mod H38 delivers as its own switch-arm64 build, or empty. */
// The note when the catalog's `version` of `id` must not replace this build's Switch package; empty otherwise.
// `installed` is the version of `id` on the card (empty: none). V1-rc5: only an installed copy is protected:
// with nothing installed the catalog's compatible version installs (Randomizer was not installable at all), and
// a catalog version newer than our Switch package replaces it.
constexpr std::string_view switch_variant(std::string_view id, std::string_view version, std::string_view installed) {
    for (const auto& variant : detail::kSwitchVariants) {
        if (variant.id == id) {
            return !installed.empty() && compare_versions(version, variant.version) <= 0 ? variant.note
                                                                                        : std::string_view{};
        }
    }
    return {};
}

// This build's Switch package version of `id`, or empty.
constexpr std::string_view switch_variant_version(std::string_view id) {
    for (const auto& variant : detail::kSwitchVariants) {
        if (variant.id == id) {
            return variant.version;
        }
    }
    return {};
}

/*
 * Why the catalog's package must not be installed, or offered as an update, on this build -- it is known
 * incompatible, or H38 delivers its own Switch build of that mod -- or empty when nothing forbids it.
 */
constexpr std::string_view catalog_block_reason(
    std::string_view id, std::string_view version, std::string_view sha256, std::string_view installed) {
    if (const auto note = switch_variant(id, version, installed); !note.empty()) {
        return note;
    }
    if (const auto found = verdict(id, version, sha256); found.status == Status::Incompatible) {
        return found.reason;
    }
    return {};
}

constexpr std::string_view status_name(Status status) {
    switch (status) {
    case Status::Tested:
        return "tested";
    case Status::Inactive:
        return "inactive";
    case Status::Incompatible:
        return "incompatible";
    case Status::Unverified:
        break;
    }
    return "unverified";
}

/* The findings apply to the Switch build they were made on; other platforms never consult them. */
#if defined(__SWITCH__)
inline constexpr bool kApplies = true;
#else
inline constexpr bool kApplies = false;
#endif

}  // namespace dusk::mods::hardware
