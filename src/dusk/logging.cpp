#include "dusk/logging.h"

#include <tracy/Tracy.hpp>

#include <string_view>
#include <system_error>

bool StubLogEnabled = true;

using namespace std::literals::string_view_literals;

namespace {
constexpr std::string_view kStubFragments[] = {
    "is a stub"sv,
    "Unimplemented: BP register"sv,
    "Unhandled BP register"sv,
    "Unhandled XF register"sv,
    "Unhandled XF memory write"sv,
    "but selective updates are not implemented"sv,
};

bool is_for_stub_log(const std::string_view message) {
    for (const auto& fragment : kStubFragments) {
        if (message.find(fragment) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool divert_stub_messages(const borealis::log::Message& message) {
    ZoneScoped;
    if (!StubLogEnabled || !is_for_stub_log(message.text)) {
        return false;
    }
    dusk::SendToStubLog(message.level, message.module, message.text);
    return true;
}
}  // namespace

#ifdef __SWITCH__
// How much the console logs, and how eagerly. Debug if a marker file has been left on
// the card (sdmc:/switch/dusklight/log.flag) or the card cannot be asked; Info
// otherwise. Either way every line is flushed as it is written: buffering lost
// exactly the lines that mattered, since a session that aborts left a log holding
// nothing but "File logging initialized". Info rather than errors only, because an
// errors-only log was empty for every normal session; Debug is for when something
// is being chased, since it costs frames.
static borealis::LogLevel console_log_level() {
    std::error_code error;
    const bool flagged = std::filesystem::exists("sdmc:/switch/dusklight/log.flag", error);
    return (flagged || error) ? borealis::LogLevel::Debug : borealis::LogLevel::Info;
}
#endif

void dusk::InitializeLogging(
    const std::filesystem::path& cacheDir, const borealis::cli::StandardOptions& standard) {
    borealis::log::Options options{};
#if defined(__SWITCH__)
    options.level = options.flushOn = console_log_level();
#else
    options.level = borealis::LogLevel::Debug;
#endif
    options.fileDirectory = cacheDir.empty() ? std::filesystem::path{} : cacheDir / "logs";
    options.filePrefix = "dusklight";
    options.legacyFilePrefixes = {"dusk"};
    options.divert = &divert_stub_messages;
    standard.apply_to(options);
    borealis::log::init(options);
}
