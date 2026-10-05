#ifndef DUSK_ISO_VALIDATE_HPP
#define DUSK_ISO_VALIDATE_HPP

#include <borealis/disc.hpp>
#include "dusk/settings.h"

#include <cstdint>
#include <string>
#include <string_view>

#ifdef __SWITCH__
namespace dusk::sw::disc {
struct LogicalHeader;
}
#endif

namespace dusk {
enum class DiscVerificationState : uint8_t;
}

namespace dusk::iso {

enum class ValidationError : uint8_t {
    Unknown = 0,
    IOError,
    InvalidImage,
    WrongGame,
    WrongVersion,
    Canceled,
    HashMismatch,
    Success
};

using Platform = borealis::disc::Platform;

enum class Region : uint8_t {
    NorthAmerica,
    Europe,
    Japan,
    Korea,
};

using VerificationStatus = borealis::disc::Progress;

struct DiscInfo {
    Platform platform = Platform::Unknown;
    Region region = Region::NorthAmerica;
    uint8_t revision = 0;
    std::string gameId;
};

ValidationError inspect(const char* path, DiscInfo& info);
ValidationError validate(const char* path, VerificationStatus& status, DiscInfo& info);
bool isPal(const char* path);
#ifdef __SWITCH__
// H34: the same verdict as inspect(), for a header the disc selector already read
// (switch/disc_scan.hpp), so listing an image does not read it a second time.
ValidationError classify_header(const sw::disc::LogicalHeader& header, DiscInfo& info);
#endif
void log_verification_state(std::string_view path, DiscVerificationState state);

}  // namespace dusk::iso

#endif  // DUSK_ISO_VALIDATE_HPP
