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

struct SwitchVariant {
    std::string_view id;
    std::string_view note;
};

namespace detail {
using enum Status;
#define DUSK_MOD_HARDWARE(id, version, sha256, status, reason) Finding{id, version, sha256, status, reason},
#define DUSK_MOD_SWITCH_VARIANT(id, note)
inline constexpr Finding kFindings[] = {
#include "hardware_findings.inc"
};
#undef DUSK_MOD_HARDWARE
#undef DUSK_MOD_SWITCH_VARIANT
#define DUSK_MOD_HARDWARE(id, version, sha256, status, reason)
#define DUSK_MOD_SWITCH_VARIANT(id, note) SwitchVariant{id, note},
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
constexpr std::string_view switch_variant(std::string_view id) {
    for (const auto& variant : detail::kSwitchVariants) {
        if (variant.id == id) {
            return variant.note;
        }
    }
    return {};
}

/*
 * Why the catalog's package must not be installed, or offered as an update, on this build -- it is known
 * incompatible, or H38 delivers its own Switch build of that mod -- or empty when nothing forbids it.
 */
constexpr std::string_view catalog_block_reason(
    std::string_view id, std::string_view version, std::string_view sha256 = {}) {
    if (const auto note = switch_variant(id); !note.empty()) {
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
