#include "dusk/logging.h"

#include <tracy/Tracy.hpp>

#include <string_view>
#include <system_error>
#ifdef __SWITCH__
#include <switch.h>
#endif

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
// otherwise. Every line used to be flushed as it was written: buffering lost
// exactly the lines that mattered, since a session that aborts left a log holding
// nothing but "File logging initialized" (V1-rc16: see start_log_flusher for what
// replaced it in an Info session). Info rather than errors only, because an
// errors-only log was empty for every normal session; Debug is for when something
// is being chased, since it costs frames.
static borealis::LogLevel console_log_level() {
    std::error_code error;
    const bool flagged = std::filesystem::exists("sdmc:/switch/dusklight/log.flag", error);
    return (flagged || error) ? borealis::LogLevel::Debug : borealis::LogLevel::Info;
}
#endif

#ifdef __SWITCH__
// V1-rc16: in an Info session lines are no longer flushed one by one from the thread that logs. Each flush is a write
// to the SD card, and a single SD write can stall 50-150 ms (the figure from sa2-nx's notes; the background writer is
// futari-nx's idea, our own code; Boldiatti's rc14 menus froze for
// seconds while mods were scanned, each step logging). Errors and above still flush at once -- and with them every
// line buffered before them, which is what the crash paths log on their way out (std::terminate, abort, the memory
// guard) -- and this thread flushes the rest every 200 ms. A Debug session (log.flag) keeps flushing every line.
static void start_log_flusher() {
    static Thread s_thread;
    static bool s_started = false;
    if (s_started) {
        return;
    }
    // Low priority, any core but the game's: it only ever waits on the card. 0x3B, not 0x3F: the emulator refused
    // 0x3F (rc 0xe001, invalid priority) and the thread never ran, so Info logs flushed only on errors (V1-PGO).
    if (R_FAILED(threadCreate(
            &s_thread,
            [](void*) {
                for (;;) {
                    svcSleepThread(200'000'000);
                    borealis::log::flush();
                }
            },
            nullptr, nullptr, 16 * 1024, 0x3B, -2)) ||
        R_FAILED(threadStart(&s_thread))) {
        return;
    }
    s_started = true;
}
#endif

void dusk::InitializeLogging(
    const std::filesystem::path& cacheDir, const borealis::cli::StandardOptions& standard) {
    borealis::log::Options options{};
#if defined(__SWITCH__)
    options.level = console_log_level();
    options.flushOn = options.level == borealis::LogLevel::Debug ? borealis::LogLevel::Debug : borealis::LogLevel::Error;
#else
    options.level = borealis::LogLevel::Debug;
#endif
    options.fileDirectory = cacheDir.empty() ? std::filesystem::path{} : cacheDir / "logs";
    options.filePrefix = "dusklight";
    options.legacyFilePrefixes = {"dusk"};
    options.divert = &divert_stub_messages;
    standard.apply_to(options);
    borealis::log::init(options);
#ifdef __SWITCH__
    if (options.flushOn > options.level) {
        start_log_flusher();
    }
#endif
}
