// aleks2 (cross-save): host test for the save_manager pieces SaveSnapshotService is built on -- the
// real save_manager.cpp and utilities.cpp, with the card, I/O and loader stubbed here so faults can
// be injected at each stage of an apply (busy card, validation, backup, GCI write, mod data,
// remount). The property under test: a failed apply never leaves a partial save and never destroys
// the previous one; the stage that failed is reported.
//
// Built and run by test-save-snapshot.sh (ASan, UBSan, _GLIBCXX_ASSERTIONS).

#include "dusk/mod_loader.hpp"
#include "dusk/mods/svc/save.hpp"
#include "dusk/save_manager.hpp"

// The Cloud Save mod's own copy of the format (its compatibility mode), checked against this one.
#include "core/dusksave.hpp"

#include <aurora/card.h>
#include <borealis/io.hpp>

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace sm = dusk::save_manager;

// ---- faults -------------------------------------------------------------------------------------

namespace {
std::string g_failAtomicReplaceTo;  // atomic_replace fails when the destination contains this
std::string g_failRenameFrom;       // rename(2) fails when the source contains this
bool g_cardIdle = true;
bool g_remountOk = true;
int g_remounts = 0;
std::map<std::string, int> g_invalidated;
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)
}  // namespace

extern "C" int rename(const char* from, const char* to) {
    using Fn = int (*)(const char*, const char*);
    static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "rename"));
    // Only the staging -> destination rename; replace_sidecars' own rollback (from "*.old") works.
    const std::string_view source{from};
    if (!g_failRenameFrom.empty() && source.find(g_failRenameFrom) != std::string_view::npos &&
        !source.ends_with(".old"))
    {
        errno = EIO;
        return -1;
    }
    return real(from, to);
}

// ---- stand-ins ----------------------------------------------------------------------------------

std::filesystem::path CachePath;

bool mDoMemCd_isCardCommNone() {
    return g_cardIdle;
}

namespace dusk::mods {
ModLoader& ModLoader::instance() {
    static ModLoader loader;
    return loader;
}
const std::vector<ShimMod>& ModLoader::mods() const {
    static const std::vector<ShimMod> mods{{{"dev.test.alpha", "1.0.0"}}};
    return mods;
}
namespace svc {
void invalidate_save(std::string_view saveName) {
    ++g_invalidated[std::string{saveName}];
}
}  // namespace svc
}  // namespace dusk::mods

extern "C" {
AuroraCardType aurora_card_get_type(s32) {
    return AURORA_CARD_GCI_DIRECTORY;
}
size_t aurora_card_get_path(const char*, AuroraCardType, s32, char*, size_t) {
    return 0;
}
bool aurora_card_remount(s32) {
    ++g_remounts;
    return g_remountOk;
}
bool aurora_card_raw_list(const char*, const char*, const char*, void (*)(const char*, void*), void*) {
    return false;
}
size_t aurora_card_raw_extract(const char*, const char*, const char*, const char*, void*, size_t) {
    return 0;
}
bool aurora_card_raw_insert(const char*, const void*, size_t, bool) {
    return false;
}
bool aurora_card_raw_delete(const char*, const char*, const char*, const char*) {
    return false;
}
size_t aurora_card_gci_filename(
    const char* game, const char* maker, const char* fileName, char* buffer, size_t capacity) {
    const std::string name = std::string{maker} + "-" + game + "-" + fileName + ".gci";
    if (name.size() + 1 > capacity) {
        return name.size() + 1;
    }
    std::memcpy(buffer, name.c_str(), name.size() + 1);
    return name.size() + 1;
}
}

namespace borealis::io {
File::~File() = default;
uint64_t File::size() const noexcept {
    return static_cast<uint64_t>(std::filesystem::file_size(*static_cast<fs::path*>(m_access)));
}
uint64_t File::read(void* buf, uint64_t len) noexcept {
    std::ifstream in{*static_cast<fs::path*>(m_access), std::ios::binary};
    in.read(static_cast<char*>(buf), static_cast<std::streamsize>(len));
    return static_cast<uint64_t>(in.gcount());
}
OpenResult open(std::string_view location, File::Mode) {
    // A leaked path per open is fine in a test; File's destructor is defaulted.
    static std::vector<std::unique_ptr<fs::path>> paths;
    if (!fs::is_regular_file(fs::path{location})) {
        return {.status = Status::NotFound, .message = "not found"};
    }
    paths.push_back(std::make_unique<fs::path>(location));
    OpenResult result{.status = Status::Ok};
    result.file = File{reinterpret_cast<SDL_IOStream*>(paths.back().get()), paths.back().get(), false};
    return result;
}
File::File(File&& other) noexcept
    : m_handle{std::exchange(other.m_handle, nullptr)}, m_access{std::exchange(other.m_access, nullptr)},
      m_writable{other.m_writable}, m_error{std::move(other.m_error)} {}
File& File::operator=(File&& other) noexcept {
    m_handle = std::exchange(other.m_handle, nullptr);
    m_access = std::exchange(other.m_access, nullptr);
    m_writable = other.m_writable;
    m_error = std::move(other.m_error);
    return *this;
}
std::string display_name(std::string_view location) {
    return fs::path{location}.filename().string();
}
bool atomic_replace(const fs::path& source, const fs::path& destination, std::string& error) {
    if (!g_failAtomicReplaceTo.empty() &&
        destination.string().find(g_failAtomicReplaceTo) != std::string::npos)
    {
        error = "injected atomic_replace failure";
        return false;
    }
    std::error_code ec;
    fs::rename(source, destination, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}
}  // namespace borealis::io

// ---- fixtures -----------------------------------------------------------------------------------

namespace {

std::vector<uint8_t> make_gci(std::string_view game, std::string_view maker, std::string_view name,
    uint8_t fill, uint16_t blocks = 2) {
    std::vector<uint8_t> gci(0x40 + size_t{blocks} * 0x2000, fill);
    std::fill(gci.begin(), gci.begin() + 0x40, 0);
    std::memcpy(gci.data(), game.data(), 4);
    std::memcpy(gci.data() + 4, maker.data(), 2);
    std::memcpy(gci.data() + 8, name.data(), name.size());
    gci[0x28] = 0x12;
    gci[0x2b] = fill;
    gci[0x38] = static_cast<uint8_t>(blocks >> 8);
    gci[0x39] = static_cast<uint8_t>(blocks);
    return gci;
}

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

void write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> text(std::string_view value) {
    return {value.begin(), value.end()};
}

struct Card {
    sm::Storage storage;
    sm::SaveIdentity identity{.maker = "01", .game = "GZ2E", .saveName = "gczelda2"};

    explicit Card(const fs::path& root) {
        fs::remove_all(root);
        storage = {.kind = sm::StorageKind::GciDirectory, .path = root / "card", .channel = 0, .mounted = true};
        fs::create_directories(storage.path);
    }
    fs::path gci() const { return storage.path / "01-GZ2E-gczelda2.gci"; }
    fs::path sidecars() const { return storage.path / "01-GZ2E-gczelda2.mods"; }
    fs::path backups() const { return storage.path / "backups"; }
    size_t backup_count() const {
        if (!fs::exists(backups())) {
            return 0;
        }
        return static_cast<size_t>(std::distance(fs::directory_iterator{backups()}, fs::directory_iterator{}));
    }
    void put(const std::vector<uint8_t>& gciBytes, const std::map<std::string, std::vector<uint8_t>>& mods) {
        write_file(gci(), gciBytes);
        fs::remove_all(sidecars());
        for (const auto& [id, data] : mods) {
            write_file(sidecars() / (id + ".json"), data);
        }
    }
    std::string digest() const {
        auto g = sm::read_save_gci(storage, identity);
        auto m = sm::read_save_mod_files(storage, identity);
        return g && m && !g.value.empty() ? sm::snapshot_digest(g.value, m.value) : std::string{};
    }
};

void reset_faults() {
    g_failAtomicReplaceTo.clear();
    g_failRenameFrom.clear();
    g_cardIdle = true;
    g_remountOk = true;
    g_remounts = 0;
    g_invalidated.clear();
}

// A snapshot the way SaveSnapshotService::capture writes it.
sm::Artifact snapshot_of(const fs::path& work, const std::vector<uint8_t>& gciBytes,
    const std::map<std::string, std::vector<uint8_t>>& mods) {
    const auto path = work / "snapshot.dusksave";
    CHECK(sm::write_dusksave(path, gciBytes, mods));
    auto artifact = sm::read_artifact(path.string());
    CHECK(artifact);
    return artifact.value;
}

// ---- tests --------------------------------------------------------------------------------------

void test_digest(const fs::path&) {
    const auto gci = make_gci("GZ2E", "01", "gczelda2", 1);
    const std::map<std::string, std::vector<uint8_t>> a{{"dev.a", text("{}")}, {"dev.b", text("[1]")}};
    std::map<std::string, std::vector<uint8_t>> b;
    b.emplace("dev.b", text("[1]"));
    b.emplace("dev.a", text("{}"));
    CHECK(sm::snapshot_digest(gci, a) == sm::snapshot_digest(gci, b));
    CHECK(sm::snapshot_digest(gci, a).size() == 64);
    auto changed = gci;
    changed.back() ^= 1;
    CHECK(sm::snapshot_digest(changed, a) != sm::snapshot_digest(gci, a));
    auto c = a;
    c["dev.b"] = text("[2]");
    CHECK(sm::snapshot_digest(gci, c) != sm::snapshot_digest(gci, a));
    // Framing: moving a byte between the id and the data must change the digest.
    const std::map<std::string, std::vector<uint8_t>> d{{"dev.ab", text("x")}};
    const std::map<std::string, std::vector<uint8_t>> e{{"dev.a", text("bx")}};
    CHECK(sm::snapshot_digest(gci, d) != sm::snapshot_digest(gci, e));
    CHECK(sm::snapshot_digest(gci, {}) != sm::snapshot_digest(gci, a));
}

void test_roundtrip(const fs::path& root) {
    reset_faults();
    Card card{root / "roundtrip"};
    const auto gci = make_gci("GZ2E", "01", "gczelda2", 7);
    const std::map<std::string, std::vector<uint8_t>> mods{{"dev.test.alpha", text("{\"v\":1}")}};
    card.put(gci, mods);
    const auto before = card.digest();
    const auto artifact = snapshot_of(root / "roundtrip-work", gci, mods);
    CHECK(artifact.kind == sm::ArtifactKind::DuskSave);
    CHECK(artifact.gci == gci);
    CHECK(artifact.modFiles == mods);
    CHECK(sm::snapshot_digest(artifact.gci, artifact.modFiles) == before);
    // Writing the same content twice gives a different container (timestamp) but the same digest.
    const auto again = snapshot_of(root / "roundtrip-work2", gci, mods);
    CHECK(sm::snapshot_digest(again.gci, again.modFiles) == before);
}

void test_apply_ok(const fs::path& root) {
    reset_faults();
    Card card{root / "ok"};
    card.put(make_gci("GZ2E", "01", "gczelda2", 1), {{"dev.test.alpha", text("old")}});
    const auto incoming = make_gci("GZ2E", "01", "gczelda2", 2);
    const std::map<std::string, std::vector<uint8_t>> mods{{"dev.test.beta", text("new")}};
    const auto artifact = snapshot_of(root / "ok-work", incoming, mods);
    const auto outcome = sm::apply_snapshot(card.storage, card.identity, artifact, false);
    CHECK(outcome.result);
    CHECK(outcome.failedStage == sm::ApplyStage::None);
    CHECK(!outcome.backup.empty() && fs::exists(outcome.backup));
    CHECK(read_file(card.gci()) == incoming);
    CHECK(fs::exists(card.sidecars() / "dev.test.beta.json"));
    CHECK(!fs::exists(card.sidecars() / "dev.test.alpha.json"));
    CHECK(card.digest() == sm::snapshot_digest(incoming, mods));
    CHECK(g_remounts == 1);
    CHECK(g_invalidated["gczelda2"] == 1);
    // The backup holds the previous save and its mod data.
    auto backup = sm::read_artifact(outcome.backup.string());
    CHECK(backup && backup.value.gci == make_gci("GZ2E", "01", "gczelda2", 1));
    CHECK(backup && backup.value.modFiles.contains("dev.test.alpha"));
}

void test_apply_new_device(const fs::path& root) {
    reset_faults();
    Card card{root / "new"};
    const auto incoming = make_gci("GZ2E", "01", "gczelda2", 3);
    const auto artifact = snapshot_of(root / "new-work", incoming, {});
    const auto outcome = sm::apply_snapshot(card.storage, card.identity, artifact, false);
    CHECK(outcome.result);
    CHECK(outcome.backup.empty());  // nothing to back up
    CHECK(read_file(card.gci()) == incoming);
}

void test_conversion(const fs::path& root) {
    reset_faults();
    Card card{root / "conv"};
    const auto local = make_gci("GZ2E", "01", "gczelda2", 1);
    card.put(local, {});
    const auto pal = make_gci("GZ2P", "01", "gczelda2", 4);
    const auto artifact = snapshot_of(root / "conv-work", pal, {});
    auto refused = sm::apply_snapshot(card.storage, card.identity, artifact, false);
    CHECK(!refused.result);
    CHECK(refused.failedStage == sm::ApplyStage::Validate);
    CHECK(refused.compatibility == sm::DiscCompatibility::RegionChange);
    CHECK(read_file(card.gci()) == local);
    CHECK(card.backup_count() == 0);
    auto accepted = sm::apply_snapshot(card.storage, card.identity, artifact, true);
    CHECK(accepted.result);
    const auto written = read_file(card.gci());
    CHECK(written.size() == pal.size() && std::memcmp(written.data(), "GZ2E", 4) == 0);
    CHECK(std::equal(written.begin() + 4, written.end(), pal.begin() + 4));
    // Wii -> GameCube is a platform change.
    const auto wii = make_gci("RZDE", "01", "gczelda2", 5);
    auto platform = sm::apply_snapshot(card.storage, card.identity, snapshot_of(root / "conv-wii", wii, {}), false);
    CHECK(platform.compatibility == sm::DiscCompatibility::PlatformChange);
    CHECK(platform.failedStage == sm::ApplyStage::Validate);
}

void test_refusals(const fs::path& root) {
    reset_faults();
    Card card{root / "refuse"};
    const auto local = make_gci("GZ2E", "01", "gczelda2", 1);
    card.put(local, {});
    for (const auto& bad : {make_gci("GZ2E", "02", "gczelda2", 2), make_gci("GALE", "01", "gczelda2", 2),
             make_gci("GZ2E", "01", "other", 2)})
    {
        auto outcome = sm::apply_snapshot(card.storage, card.identity, snapshot_of(root / "refuse-work", bad, {}), true);
        CHECK(!outcome.result);
        CHECK(outcome.failedStage == sm::ApplyStage::Validate);
    }
    // A corrupt snapshot is not even an artifact.
    write_file(root / "refuse-work" / "junk.dusksave", text("PK\x03\x04 not a zip"));
    CHECK(!sm::read_artifact((root / "refuse-work" / "junk.dusksave").string()));
    sm::Artifact truncated = snapshot_of(root / "refuse-work", make_gci("GZ2E", "01", "gczelda2", 2), {});
    truncated.gci.resize(truncated.gci.size() - 1);
    CHECK(sm::apply_snapshot(card.storage, card.identity, truncated, true).failedStage == sm::ApplyStage::Validate);
    CHECK(read_file(card.gci()) == local);
    CHECK(card.backup_count() == 0);
    CHECK(g_remounts == 0);
}

void test_busy(const fs::path& root) {
    reset_faults();
    Card card{root / "busy"};
    const auto local = make_gci("GZ2E", "01", "gczelda2", 1);
    card.put(local, {});
    g_cardIdle = false;
    auto outcome = sm::apply_snapshot(card.storage, card.identity,
        snapshot_of(root / "busy-work", make_gci("GZ2E", "01", "gczelda2", 2), {}), false);
    CHECK(outcome.failedStage == sm::ApplyStage::Busy);
    CHECK(!sm::ensure_card_idle(card.storage));
    CHECK(!sm::backup_save(card.storage, card.identity));
    CHECK(read_file(card.gci()) == local);
    // Not mounted: the game is not using the card (pre-launch), so it is never busy.
    card.storage.mounted = false;
    CHECK(sm::ensure_card_idle(card.storage));
}

void test_backup_failure(const fs::path& root) {
    reset_faults();
    Card card{root / "backupfail"};
    const auto local = make_gci("GZ2E", "01", "gczelda2", 1);
    const std::map<std::string, std::vector<uint8_t>> mods{{"dev.test.alpha", text("keep")}};
    card.put(local, mods);
    const auto artifact = snapshot_of(root / "backupfail-work", make_gci("GZ2E", "01", "gczelda2", 2), {});
    g_failAtomicReplaceTo = "backups";
    auto outcome = sm::apply_snapshot(card.storage, card.identity, artifact, false);
    CHECK(!outcome.result);
    CHECK(outcome.failedStage == sm::ApplyStage::Backup);
    CHECK(read_file(card.gci()) == local);
    CHECK(read_file(card.sidecars() / "dev.test.alpha.json") == text("keep"));
    CHECK(g_remounts == 0);
}

void test_write_failure(const fs::path& root) {
    reset_faults();
    Card card{root / "writefail"};
    const auto local = make_gci("GZ2E", "01", "gczelda2", 1);
    card.put(local, {{"dev.test.alpha", text("keep")}});
    const auto artifact = snapshot_of(root / "writefail-work", make_gci("GZ2E", "01", "gczelda2", 2),
        {{"dev.test.beta", text("new")}});
    g_failAtomicReplaceTo = ".gci";
    auto outcome = sm::apply_snapshot(card.storage, card.identity, artifact, false);
    CHECK(!outcome.result);
    CHECK(outcome.failedStage == sm::ApplyStage::Write);
    CHECK(read_file(card.gci()) == local);
    CHECK(fs::exists(card.sidecars() / "dev.test.alpha.json"));
    CHECK(!fs::exists(card.sidecars() / "dev.test.beta.json"));
    CHECK(!fs::exists(card.storage.path / "01-GZ2E-gczelda2.gci.tmp"));
    CHECK(card.backup_count() == 1);  // the backup was made before the write
}

void test_mod_data_failure(const fs::path& root) {
    reset_faults();
    Card card{root / "modfail"};
    const auto local = make_gci("GZ2E", "01", "gczelda2", 1);
    card.put(local, {{"dev.test.alpha", text("keep")}});
    const auto before = card.digest();
    const auto artifact = snapshot_of(root / "modfail-work", make_gci("GZ2E", "01", "gczelda2", 2),
        {{"dev.test.beta", text("new")}});
    // replace_sidecars stages the new directory, moves the old one aside and renames the staging
    // into place: make that last rename fail.
    g_failRenameFrom = ".replace-";
    auto outcome = sm::apply_snapshot(card.storage, card.identity, artifact, false);
    g_failRenameFrom.clear();
    CHECK(!outcome.result);
    CHECK(outcome.failedStage == sm::ApplyStage::ModData);
    CHECK(read_file(card.gci()) == local);  // the previous GCI was put back
    CHECK(read_file(card.sidecars() / "dev.test.alpha.json") == text("keep"));
    CHECK(card.digest() == before);
    CHECK(card.backup_count() == 1);

    // Same failure with no previous save: the new GCI must not stay behind alone.
    Card empty{root / "modfail-empty"};
    g_failRenameFrom = ".replace-";
    auto second = sm::apply_snapshot(empty.storage, empty.identity, artifact, false);
    g_failRenameFrom.clear();
    CHECK(second.failedStage == sm::ApplyStage::ModData);
    CHECK(!fs::exists(empty.gci()));
}

void test_remount_failure(const fs::path& root) {
    reset_faults();
    Card card{root / "remount"};
    card.put(make_gci("GZ2E", "01", "gczelda2", 1), {});
    const auto incoming = make_gci("GZ2E", "01", "gczelda2", 2);
    g_remountOk = false;
    auto outcome = sm::apply_snapshot(card.storage, card.identity, snapshot_of(root / "remount-work", incoming, {}), false);
    CHECK(!outcome.result);
    CHECK(outcome.failedStage == sm::ApplyStage::Remount);
    // Documented: the new save is complete on disk; the game must restart before playing.
    CHECK(read_file(card.gci()) == incoming);
    CHECK(g_invalidated["gczelda2"] == 1);
}

void test_mod_interop(const fs::path& root) {
    // The mod packs and reads .dusksave and computes the digest on its own on official builds; the
    // host must accept what it packs, and the other way round, with the same digest.
    reset_faults();
    namespace ds = crosssave::dusksave;
    for (int variant = 0; variant < 3; ++variant) {
        const auto gci = make_gci(variant == 2 ? "GZ2P" : "GZ2E", "01", "gczelda2", static_cast<uint8_t>(10 + variant),
            static_cast<uint16_t>(1 + variant));
        std::map<std::string, std::vector<uint8_t>> mods;
        if (variant > 0) {
            mods["dev.test.alpha"] = text("{\"a\":1}");
            mods["dev.test.zeta"] = text("[\"z\"]");
        }
        CHECK(ds::digest(gci, mods) == sm::snapshot_digest(gci, mods));
        // Mod -> host.
        const auto packed = ds::pack(gci, mods, "2026-10-16T00:00:00Z");
        CHECK(packed.has_value());
        const auto modFile = root / "interop" / ("mod-" + std::to_string(variant) + ".dusksave");
        write_file(modFile, *packed);
        auto hostRead = sm::read_artifact(modFile.string());
        CHECK(hostRead && hostRead.value.kind == sm::ArtifactKind::DuskSave);
        CHECK(hostRead && hostRead.value.gci == gci && hostRead.value.modFiles == mods);
        // Host -> mod.
        const auto hostFile = root / "interop" / ("host-" + std::to_string(variant) + ".dusksave");
        CHECK(sm::write_dusksave(hostFile, gci, mods));
        std::string error;
        const auto modRead = ds::read(read_file(hostFile), error);
        CHECK(modRead && modRead->gci == gci && modRead->modFiles == mods);
        CHECK(modRead && ds::digest(modRead->gci, modRead->modFiles) == sm::snapshot_digest(gci, mods));
        // Same parse and compatibility rules.
        const auto header = ds::parse_gci(gci);
        auto hostHeader = sm::parse_gci(gci);
        CHECK(header && hostHeader && header->game == hostHeader.value.game && header->saveName == hostHeader.value.saveName);
        const sm::SaveIdentity identity{.maker = "01", .game = "GZ2E", .saveName = "gczelda2"};
        CHECK(static_cast<int>(ds::compatibility(*header, "GZ2E", "01")) ==
              static_cast<int>(sm::disc_compatibility(hostHeader.value, identity)));
    }
    // A bare GCI reads as a snapshot without mod data, as in the host.
    std::string error;
    CHECK(ds::read(make_gci("GZ2E", "01", "gczelda2", 3), error).has_value());
    CHECK(!ds::read(text("PK\x03\x04 broken"), error));
    CHECK(ds::sync_key("RZDE", "01", "gczelda2") == "GZ2-01-gczelda2");
}

void test_saves_window_unchanged(const fs::path& root) {
    // import_artifact (the Saves window) still accepts a region change: it asks the player itself.
    reset_faults();
    Card card{root / "window"};
    card.put(make_gci("GZ2E", "01", "gczelda2", 1), {});
    const auto artifact = snapshot_of(root / "window-work", make_gci("GZ2P", "01", "gczelda2", 4), {});
    CHECK(sm::import_artifact(card.storage, card.identity, artifact, sm::ModDataAction::Keep));
    CHECK(sm::create_backup(card.storage, card.identity));
    // Retention: never more than kDefaultBackupRetention backups for one save.
    for (int i = 0; i < 14; ++i) {
        CHECK(sm::create_backup(card.storage, card.identity));
    }
    CHECK(card.backup_count() <= static_cast<size_t>(sm::kDefaultBackupRetention));
    auto backup = sm::backup_save(card.storage, card.identity);
    CHECK(backup && !backup.value.empty());
    Card none{root / "window-none"};
    auto nothing = sm::backup_save(none.storage, none.identity);
    CHECK(nothing && nothing.value.empty());
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path{argv[1]} : fs::temp_directory_path() / "save-snapshot-test";
    fs::create_directories(root);
    CachePath = root / "cache";
    test_digest(root);
    test_roundtrip(root);
    test_apply_ok(root);
    test_apply_new_device(root);
    test_conversion(root);
    test_refusals(root);
    test_busy(root);
    test_backup_failure(root);
    test_write_failure(root);
    test_mod_data_failure(root);
    test_remount_failure(root);
    test_saves_window_unchanged(root);
    test_mod_interop(root);
    std::printf("save_snapshot_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("save_snapshot_test: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
