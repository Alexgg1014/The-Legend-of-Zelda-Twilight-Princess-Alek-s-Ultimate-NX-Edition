// aleks2 (DM1-b): the update journal (src/dusk/mods/loader/update_journal.hpp) on the host.
// Interruptions are simulated by leaving the files as a power cut at each step would.
#include "dusk/mods/loader/update_journal.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace dusk::mods;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream{path, std::ios::binary | std::ios::trunc} << text;
}
std::string get(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}
// A "package" is readable when it starts with PK, like a zip.
bool readable(const fs::path& path) {
    return get(path).rfind("PK", 0) == 0;
}
bool replace_ok(const fs::path& from, const fs::path& to, std::string&) {
    std::error_code ec;
    fs::remove(to, ec);
    fs::rename(from, to);
    return true;
}
bool replace_fail(const fs::path&, const fs::path&, std::string& error) {
    error = "simulated write failure";
    return false;
}

struct Fixture {
    fs::path dir;
    journal::Entry entry;
    explicit Fixture(const fs::path& root, const char* name) : dir{root / name} {
        fs::remove_all(dir);
        fs::create_directories(dir);
        entry = {.id = "dev.example.mod", .fromVersion = "1.1.1", .toVersion = "1.1.2",
            .destination = dir / "dev.example.mod.dusk", .backup = dir / ".backup" / "dev.example.mod.dusk"};
        put(entry.backup, "PK old");
        put(entry.destination, "PK new");
    }
};
}  // namespace

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? argv[1] : "update_journal_test";
    fs::create_directories(root);

    {  // round trip, and ids that could leave the directory are refused
        Fixture f{root, "roundtrip"};
        std::string error;
        CHECK(journal::write(f.dir, f.entry, error));
        const auto all = journal::read_all(f.dir);
        CHECK(all.size() == 1 && all[0].toVersion == "1.1.2" && all[0].backup == f.entry.backup);
        auto bad = f.entry;
        bad.id = "../escape";
        CHECK(!journal::write(f.dir, bad, error));
    }
    {  // a session that ended mid-download leaves .part files; the start removes them
        Fixture f{root, "stale"};
        put(f.dir / ".staging" / "x.dusk.part", "partial");
        const auto actions = journal::recover_at_start(f.dir, "", readable, replace_ok);
        CHECK(!fs::exists(f.dir / ".staging" / "x.dusk.part"));
        CHECK(actions.size() == 1);
    }
    {  // the previous boot crashed in the updated mod: the backup comes back before it loads
        Fixture f{root, "crash"};
        std::string error;
        CHECK(journal::write(f.dir, f.entry, error));
        const auto actions = journal::recover_at_start(f.dir, "dev.example.mod", readable, replace_ok);
        CHECK(get(f.entry.destination) == "PK old");
        CHECK(journal::read_all(f.dir).empty());
        CHECK(actions.size() == 1 && actions[0].restored);
        CHECK(actions[0].message.find("data") != std::string::npos);  // says data is not rolled back
    }
    {  // power cut while the new package was being written: truncated, unreadable -> restored
        Fixture f{root, "truncated"};
        std::string error;
        CHECK(journal::write(f.dir, f.entry, error));
        put(f.entry.destination, "");
        journal::recover_at_start(f.dir, "", readable, replace_ok);
        CHECK(get(f.entry.destination) == "PK old");
    }
    {  // power cut after the journal, before the swap: old package still there and readable;
       // nothing is touched at start, and settling commits (what runs is a valid version)
        Fixture f{root, "before-swap"};
        put(f.entry.destination, "PK old");
        std::string error;
        CHECK(journal::write(f.dir, f.entry, error));
        const auto actions = journal::recover_at_start(f.dir, "", readable, replace_ok);
        CHECK(actions.empty());
        CHECK(journal::read_all(f.dir).size() == 1);
        journal::settle_after_boot(f.dir, [](std::string_view) { return journal::Outcome::Running; }, replace_ok);
        CHECK(journal::read_all(f.dir).empty());
        CHECK(get(f.entry.destination) == "PK old");
    }
    {  // healthy start: the new version runs and is committed
        Fixture f{root, "commit"};
        std::string error;
        CHECK(journal::write(f.dir, f.entry, error));
        CHECK(journal::recover_at_start(f.dir, "", readable, replace_ok).empty());
        journal::settle_after_boot(f.dir, [](std::string_view) { return journal::Outcome::Running; }, replace_ok);
        CHECK(get(f.entry.destination) == "PK new");
        CHECK(journal::read_all(f.dir).empty());
    }
    {  // the new version failed to load: the old one is put back for the next start
        Fixture f{root, "failed"};
        std::string error;
        CHECK(journal::write(f.dir, f.entry, error));
        const auto actions = journal::settle_after_boot(
            f.dir, [](std::string_view) { return journal::Outcome::Failed; }, replace_ok);
        CHECK(get(f.entry.destination) == "PK old");
        CHECK(actions.size() == 1 && actions[0].restored);
        CHECK(journal::read_all(f.dir).empty());
    }
    {  // disabled by the player: no verdict yet, the entry waits
        Fixture f{root, "not-started"};
        std::string error;
        CHECK(journal::write(f.dir, f.entry, error));
        journal::settle_after_boot(f.dir, [](std::string_view) { return journal::Outcome::NotStarted; }, replace_ok);
        CHECK(journal::read_all(f.dir).size() == 1);
        CHECK(get(f.entry.destination) == "PK new");
    }
    {  // the backup is gone: nothing restored, the entry and the failing package stay, the message says so
        Fixture f{root, "no-backup"};
        std::string error;
        CHECK(journal::write(f.dir, f.entry, error));
        fs::remove(f.entry.backup);
        const auto actions = journal::recover_at_start(f.dir, "dev.example.mod", readable, replace_ok);
        CHECK(actions.size() == 1 && !actions[0].restored);
        CHECK(journal::read_all(f.dir).size() == 1);
    }
    {  // the swap itself fails: no .part left behind, the entry stays for the next start
        Fixture f{root, "replace-fails"};
        std::string error;
        CHECK(journal::write(f.dir, f.entry, error));
        journal::settle_after_boot(f.dir, [](std::string_view) { return journal::Outcome::Failed; }, replace_fail);
        CHECK(!fs::exists(f.dir / ".staging" / "dev.example.mod-restore.dusk.part"));
        CHECK(journal::read_all(f.dir).size() == 1);
        CHECK(get(f.entry.destination) == "PK new");
    }
    {  // a damaged journal file is reported and left alone, never acted on
        Fixture f{root, "bad-json"};
        put(journal::directory(f.dir) / "dev.example.mod.json", "{not json");
        std::vector<std::string> problems;
        CHECK(journal::read_all(f.dir, &problems).empty() && problems.size() == 1);
        const auto actions = journal::recover_at_start(f.dir, "dev.example.mod", readable, replace_ok);
        CHECK(actions.size() == 1 && !actions[0].restored);
        CHECK(get(f.entry.destination) == "PK new");
    }

    if (failures == 0) {
        std::puts("update_journal_test: all checks passed");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
