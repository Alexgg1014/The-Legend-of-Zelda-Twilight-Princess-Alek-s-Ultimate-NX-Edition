#include "iso_validate.hpp"

#include <borealis/disc.hpp>

#include <array>
#include <atomic>
#include <string_view>

#ifdef __SWITCH__
#include <switch/disc_header.hpp>

#include <string>
#endif

#include "dusk/logging.h"
#include "dusk/settings.h"

namespace {

const char* verification_state_name(dusk::DiscVerificationState state) noexcept {
    switch (state) {
    case dusk::DiscVerificationState::Success:
        return "verified";
    case dusk::DiscVerificationState::HashMismatch:
        return "hash mismatch";
    case dusk::DiscVerificationState::Unknown:
    default:
        return "unknown";
    }
}

}  // namespace

namespace dusk::iso {
namespace {



constexpr auto AcceptedDiscs = std::to_array<borealis::disc::AcceptedDisc>({
    {
        .gameId = "GZ2E01",
        .expectedHash = borealis::disc::parse_xxh3_128("14e886f08e548a000afde98a3195e788"),
    },
    {
        .gameId = "GZ2J01",
        .expectedHash = borealis::disc::parse_xxh3_128("5967dc7a6a553652f4d2050aeef6f368"),
    },
    {
        .gameId = "GZ2P01",
        .expectedHash = borealis::disc::parse_xxh3_128("9ef597588b0035ca9e91b333fa9a8a7e"),
    },
    {
        .gameId = "RZDE01",
        .revision = 0,
        .expectedHash = borealis::disc::parse_xxh3_128("b3d91fbea59e5c66934d04c01566728e"),
    },
    {
        .gameId = "RZDE01",
        .revision = 2,
        .expectedHash = borealis::disc::parse_xxh3_128("c3ec420921a1b36d6ae43f576491d25c"),
    },
    {
        .gameId = "RZDJ01",
        .expectedHash = borealis::disc::parse_xxh3_128("d3866821c7fc6999e6e8bbef8b6875aa"),
    },
    {
        .gameId = "RZDP01",
        .expectedHash = borealis::disc::parse_xxh3_128("6095a924a57e5fb4294ac96fb85a09a1"),
    },
});

constexpr auto RecognizedGameIds = std::to_array<std::string_view>({"RZDK01"});

Region region_from_game_id(std::string_view gameId) noexcept {
    if (gameId.size() >= 4) {
        switch (gameId[3]) {
        case 'P':
            return Region::Europe;
        case 'J':
            return Region::Japan;
        case 'K':
            return Region::Korea;
        default:
            break;
        }
    }
    return Region::NorthAmerica;
}

#ifdef __SWITCH__
// The console has no nod (it is Rust), and reading several gigabytes off the card to
// hash a whole image before the game will start is not something a console should
// do. So here validation is the image's header and nothing more: it already says
// which game, which disc and which revision, and a damaged image shows itself the
// moment something is read from it.
//
// The header is the disc's, not the file's. For an .iso the two are the same bytes;
// for an .rvz, .wia or .gcz the file begins with the container's own header and the
// disc's lies inside the compressed data. So the image is opened through the same
// container reader the game reads the disc with (platforms/switch/source/
// disc_container.cpp), and the first 0x20 bytes of the logical disc are taken from
// it. Every supported container then goes through the one check below.
ValidationError classify_switch(const sw::disc::LogicalHeader& header, DiscInfo& info) {
    switch (header.status) {
    case sw::disc::HeaderStatus::Unreadable:
        return ValidationError::IOError;
    case sw::disc::HeaderStatus::NotADisc:
    case sw::disc::HeaderStatus::Unsupported:
    case sw::disc::HeaderStatus::Damaged:
        // There is no result code of its own for these; the log says which it was and
        // what to do about it.
        DuskLog.warn("{}", header.message);
        return ValidationError::InvalidImage;
    case sw::disc::HeaderStatus::Ok:
        break;
    }

    info.platform = header.console == sw::disc::Console::Wii ? Platform::Wii : Platform::GameCube;
    info.region = region_from_game_id(header.game_id);
    info.revision = header.revision;
    info.gameId = header.game_id;

    // This game, this disc and this revision.
    for (const auto& accepted : AcceptedDiscs) {
        if (accepted.gameId == header.game_id && accepted.discNumber == header.disc_number &&
            accepted.revision == header.revision) {
            return ValidationError::Success;
        }
    }
    // The right game, in a pressing that is not supported: the player has the disc,
    // just not that version of it, which is worth saying separately.
    for (const std::string_view id : RecognizedGameIds) {
        if (id == header.game_id) {
            return ValidationError::WrongVersion;
        }
    }
    return ValidationError::WrongGame;
}

ValidationError inspect_switch(const char* path, DiscInfo& info) {
    return classify_switch(sw::disc::read_logical_header(path), info);
}
#else
constexpr borealis::disc::Catalog DiscCatalog{
    .acceptedDiscs = AcceptedDiscs,
    .recognizedGameIds = RecognizedGameIds,
};

ValidationError validation_error(borealis::disc::Status status) noexcept {
    switch (status) {
    case borealis::disc::Status::Success:
        return ValidationError::Success;
    case borealis::disc::Status::IOError:
        return ValidationError::IOError;
    case borealis::disc::Status::InvalidImage:
        return ValidationError::InvalidImage;
    case borealis::disc::Status::UnknownGame:
        return ValidationError::WrongGame;
    case borealis::disc::Status::UnsupportedVersion:
        return ValidationError::WrongVersion;
    case borealis::disc::Status::Canceled:
        return ValidationError::Canceled;
    case borealis::disc::Status::HashMismatch:
        return ValidationError::HashMismatch;
    case borealis::disc::Status::Failed:
    default:
        return ValidationError::Unknown;
    }
}



void update_info(const borealis::disc::Result& result, DiscInfo& info) {
    if (!result.metadata.gameId.empty()) {
        info.platform = result.metadata.platform;
        info.region = region_from_game_id(result.metadata.gameId);
        info.revision = result.metadata.revision;
        info.gameId = result.metadata.gameId;
    }
}

#endif

}  // namespace

ValidationError validate(const char* path, VerificationStatus& status, DiscInfo& info) {
#ifdef __SWITCH__
    // Only check header on HOS to avoid overhead and pain
    status.bytesRead.store(1, std::memory_order_relaxed);
    status.bytesTotal.store(1, std::memory_order_relaxed);
    return inspect_switch(path, info);
#else
    const auto result = borealis::disc::verify(
        path == nullptr ? std::string_view{} : std::string_view{path}, DiscCatalog, &status);
    update_info(result, info);
    return validation_error(result.status);
#endif
}

ValidationError inspect(const char* path, DiscInfo& info) {
#ifdef __SWITCH__
    return inspect_switch(path, info);
#else
    const auto result = borealis::disc::inspect(
        path == nullptr ? std::string_view{} : std::string_view{path}, DiscCatalog);
    update_info(result, info);
    return validation_error(result.status);
#endif
}

#ifdef __SWITCH__
ValidationError classify_header(const sw::disc::LogicalHeader& header, DiscInfo& info) {
    return classify_switch(header, info);
}
#endif

bool isPal(const char* path) {
    DiscInfo info{};
    return inspect(path, info) == ValidationError::Success && info.region == Region::Europe;
}

void log_verification_state(std::string_view path, DiscVerificationState state) {
    const std::string pathText = path.empty() ? "<none>" : std::string(path);
    DuskLog.info(
        "Disc verification status: {} (path: {})", verification_state_name(state), pathText);
}
}  // namespace dusk::iso
