// H37: safe mode (src/dusk/mods/loader/boot_guard.cpp) with real crashes.
//
// Each "boot" that crashes is a child process that does what the loader does -- begin_boot(),
// then enter() a mod's step -- and then dies by abort(), as a crash in mod code would end the
// game. The parent is the next boot, and checks what it is told:
//   * a clean boot leaves nothing behind;
//   * a crash in mod_initialize, and one in constructors, each quarantine that mod with the right
//     category and stage, and the quarantine holds across boots until release();
//   * a crash with no marker is not pinned on any mod; two in a row and the next boot is without
//     mods, and the one after that tries mods again; end_boot() in between resets the count;
//   * the player's "start without mods" applies to one boot;
//   * failures that return are reported with their category and never quarantined; the report
//     keeps at most kMaxIncidents;
//   * a damaged report.json or a half-written temporary file does not fail a boot;
//   * every loader error code maps to a category, and the nine categories round-trip.
#include "dusk/mods/loader/boot_guard.hpp"
#include "dusk/mods/loader/elf_loader.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>

using namespace dusk::mods::guard;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

// A boot that dies: runs `body` in a child process, which then aborts.
void crashing_boot(const fs::path& dir, const std::function<void(BootGuard&)>& body) {
    const pid_t pid = fork();
    if (pid == 0) {
        BootGuard guard(dir);
        guard.begin_boot();
        body(guard);
        std::abort();
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

void clean_boot(const fs::path& dir) {
    BootGuard guard(dir);
    const auto start = guard.begin_boot();
    CHECK(!start.crashed && !start.unattributedCrash && !start.withoutMods);
    guard.enter("dev.test.fine", Stage::Load);
    guard.leave();
    guard.enter("dev.test.fine", Stage::Initialize);
    guard.leave();
    guard.end_boot();
    CHECK(!fs::exists(dir / "pending.json") && !fs::exists(dir / "boot.marker"));
}

void crashes_in_mods(const fs::path& dir) {
    crashing_boot(dir, [](BootGuard& g) { g.enter("dev.test.crashy", Stage::Initialize); });
    {
        BootGuard guard(dir);
        const auto start = guard.begin_boot();
        CHECK(start.crashed && start.crashed->modId == "dev.test.crashy");
        CHECK(start.crashed && start.crashed->category == Category::CrashDuringInit);
        CHECK(start.crashed && start.crashed->stage == "initialize");
        CHECK(!start.withoutMods && !start.unattributedCrash);
        std::string why;
        CHECK(guard.quarantined("dev.test.crashy", &why) && why.find("crashed") != std::string::npos);
        CHECK(!guard.quarantined("dev.test.fine"));
        guard.end_boot();
    }
    crashing_boot(dir, [](BootGuard& g) {
        g.enter("dev.test.ctor", Stage::Load);
        g.leave();
        g.enter("dev.test.ctor", Stage::Constructors);
    });
    {
        BootGuard guard(dir);
        const auto start = guard.begin_boot();
        CHECK(start.crashed && start.crashed->modId == "dev.test.ctor");
        CHECK(start.crashed && start.crashed->category == Category::ConstructorFailure);
        CHECK(guard.quarantined("dev.test.crashy"));  // still, a boot later
        CHECK(guard.quarantined("dev.test.ctor"));
        guard.release("dev.test.crashy");
        CHECK(!guard.quarantined("dev.test.crashy"));
        guard.end_boot();
    }
    BootGuard guard(dir);
    guard.begin_boot();
    CHECK(!guard.quarantined("dev.test.crashy") && guard.quarantined("dev.test.ctor"));
    guard.release("dev.test.ctor");
    guard.end_boot();
    std::printf("mod crashes  initialize and constructor crashes quarantine the mod, across boots, until released\n");
}

void unattributed_crashes(const fs::path& dir) {
    crashing_boot(dir, [](BootGuard&) {});
    {
        BootGuard guard(dir);
        const auto start = guard.begin_boot();
        CHECK(start.unattributedCrash && !start.crashed && !start.withoutMods);
        guard.end_boot();  // this one got through: the count starts again
    }
    crashing_boot(dir, [](BootGuard&) {});
    {
        BootGuard guard(dir);
        const auto start = guard.begin_boot();
        CHECK(start.unattributedCrash && !start.withoutMods);  // one in a row, not two
    }  // ...and this boot crashes too (no end_boot)
    {
        BootGuard guard(dir);
        const auto start = guard.begin_boot();
        CHECK(start.unattributedCrash && start.withoutMods);
        CHECK(start.withoutModsReason.find("2 times in a row") != std::string::npos);
        CHECK(guard.quarantine().empty());  // nothing was pinned on a mod
        guard.end_boot();
    }
    BootGuard guard(dir);
    const auto start = guard.begin_boot();
    CHECK(!start.withoutMods && !start.unattributedCrash);
    guard.end_boot();
    std::printf("no marker    two unpinned boot crashes in a row: one boot without mods, then mods again\n");
}

void player_asks(const fs::path& dir) {
    {
        BootGuard guard(dir);
        guard.begin_boot();
        guard.request_boot_without_mods();
        guard.end_boot();
    }
    {
        BootGuard guard(dir);
        const auto start = guard.begin_boot();
        CHECK(start.withoutMods && guard.without_mods());
        CHECK(start.withoutModsReason.find("you asked") != std::string::npos);
        guard.end_boot();
    }
    BootGuard guard(dir);
    CHECK(!guard.begin_boot().withoutMods);
    guard.end_boot();
    std::printf("player       'start without mods' applies to exactly one boot\n");
}

void reports(const fs::path& dir) {
    BootGuard guard(dir);
    guard.begin_boot();
    for (int c = 0; c <= static_cast<int>(Category::HookConflict); ++c) {
        guard.report({"dev.test.failing", static_cast<Category>(c), "load", "message " + std::to_string(c)});
    }
    CHECK(!guard.quarantined("dev.test.failing"));
    auto incidents = guard.incidents();
    CHECK(!incidents.empty() && incidents.back().category == Category::HookConflict);
    CHECK(!incidents.empty() && incidents.back().boot == guard.boot());
    for (size_t i = 0; i < BootGuard::kMaxIncidents + 10; ++i) {
        guard.report({"dev.test.noisy", Category::MissingImport, "load", "again"});
    }
    CHECK(guard.incidents().size() == BootGuard::kMaxIncidents);
    guard.end_boot();
    std::printf("reports      returned failures reported by category, never quarantined; report bounded\n");
}

void damaged_files(const fs::path& dir) {
    std::ofstream(dir / "report.json") << "{ this is not json";
    std::ofstream(dir / "report.json.tmp") << "half a file";
    std::ofstream(dir / "pending.json") << "[1, 2";
    BootGuard guard(dir);
    const auto start = guard.begin_boot();  // must not throw
    CHECK(!start.crashed);
    guard.enter("dev.test.after", Stage::Initialize);
    guard.leave();
    guard.end_boot();
    BootGuard again(dir);
    CHECK(again.begin_boot().boot >= 1);
    again.end_boot();
    std::printf("damage       a corrupt report or marker never fails a boot\n");
}

void categories() {
    using dusk::mods::elf::Error;
    for (int e = 0; e <= static_cast<int>(Error::ProtectFailed); ++e) {
        const auto code = dusk::mods::elf::error_code(static_cast<Error>(e));
        (void)category_for_load_error(code);  // every code maps somewhere
    }
    CHECK(category_for_load_error("missing_symbol") == Category::MissingImport);
    CHECK(category_for_load_error("denied_symbol") == Category::MissingImport);
    CHECK(category_for_load_error("unsupported_relocation") == Category::UnsupportedRelocation);
    CHECK(category_for_load_error("needed_rejected") == Category::MissingDependency);
    CHECK(category_for_load_error("tls_unsupported") == Category::Validation);
    CHECK(category_for_load_error("truncated") == Category::Validation);
    for (int c = 0; c <= static_cast<int>(Category::RuntimeFailure); ++c) {
        const auto category = static_cast<Category>(c);
        CHECK(category_from_code(category_code(category)) == category);
        CHECK(!category_text(category).empty());
    }
    CHECK(!category_from_code("no_such_category"));
    std::printf("categories   nine categories round-trip; every loader error code has one\n");
}

// A started mod's fail_mod after the boot (H37-c5): recorded and quarantined at once, so the crash that can follow
// the teardown does not become a boot loop, and the next boot does not load it.
void runtime_failures(const fs::path& dir) {
    {
        BootGuard guard(dir);
        guard.begin_boot();
        guard.end_boot();  // the boot window is over, as when Randomizer failed 30 s into a game
        Incident incident{"dev.test.runtime", Category::CrashDuringInit, "runtime", "Failed to get data directory"};
        CHECK(guard.record_runtime_failure(incident));
        CHECK(!guard.record_runtime_failure(incident));  // a repeated fail_mod writes nothing more
        CHECK(guard.quarantined("dev.test.runtime"));
        size_t count = 0;
        for (const auto& i : guard.incidents()) {
            if (i.modId == "dev.test.runtime") {
                ++count;
                CHECK(i.category == Category::RuntimeFailure && i.stage == "runtime" &&
                      i.message == "Failed to get data directory" && i.boot == guard.boot());
            }
        }
        CHECK(count == 1);
        // An InitializeFailure of another mod is untouched and not quarantined.
        guard.report({"dev.test.init", Category::InitializeFailure, "initialize", "mod_initialize returned an error"});
        CHECK(!guard.quarantined("dev.test.init"));
    }
    // The game dies during the teardown: no marker names the mod, the report already does.
    crashing_boot(dir, [](BootGuard&) {});
    {
        BootGuard guard(dir);
        const auto start = guard.begin_boot();
        CHECK(start.unattributedCrash && !start.crashed && !start.withoutMods);  // not CrashDuringInit
        std::string why;
        CHECK(guard.quarantined("dev.test.runtime", &why));  // the next boot does not load it
        CHECK(why.find("Failed to get data directory") != std::string::npos);
        CHECK(guard.quarantine().size() == 1);
        guard.end_boot();
        guard.release("dev.test.runtime");  // turned off and on again
        CHECK(!guard.quarantined("dev.test.runtime"));
        CHECK(guard.incidents().size() == 2);  // the history stays
        // And it may fail, be recorded and be released again.
        CHECK(guard.record_runtime_failure({"dev.test.runtime", Category::RuntimeFailure, "runtime", "again"}));
    }
    {
        BootGuard guard(dir);
        guard.request_boot_without_mods();  // Next Start Without Mods keeps working
        const auto start = guard.begin_boot();
        CHECK(start.withoutMods && guard.quarantined("dev.test.runtime"));
        guard.end_boot();
    }
    std::printf("runtime      fail_mod after the boot: one incident, quarantined before the teardown, released by the player\n");
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "h37-boot-guard";
    fs::remove_all(root);
    clean_boot(root / "clean");
    crashes_in_mods(root / "mods");
    unattributed_crashes(root / "unattributed");
    player_asks(root / "player");
    reports(root / "reports");
    damaged_files(root / "damaged");
    runtime_failures(root / "runtime");
    categories();
    if (g_failures) {
        std::fprintf(stderr, "boot_guard_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("boot_guard_test: all checks passed\n");
    return 0;
}
