// H37-c4: the services a native mod imports, read from its file and judged against the build.
// src/dusk/mods/loader/mod_services.cpp and src/dusk/mods/svc/file_availability.hpp, on the host.
//
//   * the file service exists where there is no picker (kFileServiceWithoutPicker's rule);
//   * ServiceImport records are read from a synthetic ELF's "modmeta" section: required, optional,
//     versions, nothing for a library without the section, an error for every malformed shape;
//   * a missing required service blocks; a missing optional one is listed but does not block;
//     a minor that is too new, a major that is too new and a mod built for an older major block,
//     each with the registry's own wording; another mod's service is not judged here;
//   * the Dry Run report says so: "services" per import, and "ok":false when one blocks;
//   * with H37_RANDOMIZER_SO (the library build-h37-test-mods.sh makes) Randomizer's real imports are
//     read and all satisfied by the services this Switch build offers.
#include "dusk/mods/loader/mod_services.hpp"
#include "dusk/mods/svc/file_availability.hpp"


#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <regex>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace dusk::mods;

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

const std::string kPrefix = "dev.twilitrealm.dusklight.";

// --- a synthetic ELF64 with a "modmeta" section -------------------------------------------------
void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
    v[at] = x & 0xff;
    v[at + 1] = x >> 8;
}
void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    put16(v, at, x & 0xffff);
    put16(v, at + 2, x >> 16);
}
void put64(std::vector<uint8_t>& v, size_t at, uint64_t x) {
    put32(v, at, static_cast<uint32_t>(x));
    put32(v, at + 4, static_cast<uint32_t>(x >> 32));
}

std::vector<uint8_t> import_record(const std::string& id, uint16_t major, uint16_t minMinor, bool optional) {
    std::vector<uint8_t> r(80, 0);
    put16(r, 0, 80);
    r[2] = 2;  // MOD_META_IMPORT
    r[3] = optional ? 1 : 0;
    put16(r, 4, major);
    put16(r, 6, minMinor);
    std::memcpy(&r[16], id.data(), id.size());
    return r;
}

std::vector<uint8_t> header_record() {
    std::vector<uint8_t> r(8, 0);
    put16(r, 0, 8);
    r[2] = 1;  // MOD_META_HEADER
    put32(r, 4, 1);
    return r;
}

// sectionName: the name of the section the records go in ("modmeta" unless a test says otherwise).
std::vector<uint8_t> make_elf(const std::vector<uint8_t>& meta, const std::string& sectionName = "modmeta",
    bool withSections = true) {
    const std::string names = std::string("\0", 1) + sectionName + std::string("\0", 1) + ".shstrtab" + std::string("\0", 1);
    const size_t metaAt = 64;
    const size_t namesAt = metaAt + meta.size();
    const size_t shoff = (namesAt + names.size() + 7) & ~size_t{7};
    std::vector<uint8_t> f(shoff + 3 * 64, 0);
    std::memcpy(&f[0], "\x7f" "ELF", 4);
    f[4] = 2;
    f[5] = 1;
    f[6] = 1;
    put16(f, 0x10, 3);   // ET_DYN
    put16(f, 0x12, 183); // EM_AARCH64
    put32(f, 0x14, 1);
    if (withSections) {
        put64(f, 0x28, shoff);
        put16(f, 0x3A, 64);
        put16(f, 0x3C, 3);
        put16(f, 0x3E, 2);
    }
    std::memcpy(&f[metaAt], meta.data(), meta.size());
    std::memcpy(&f[namesAt], names.data(), names.size());
    // section 1: the metadata
    put32(f, shoff + 64, 1);                       // name: offset 1
    put32(f, shoff + 64 + 4, 1);                   // SHT_PROGBITS
    put64(f, shoff + 64 + 0x18, metaAt);
    put64(f, shoff + 64 + 0x20, meta.size());
    // section 2: the names
    put32(f, shoff + 128, static_cast<uint32_t>(1 + sectionName.size() + 1));
    put32(f, shoff + 128 + 4, 3);                  // SHT_STRTAB
    put64(f, shoff + 128 + 0x18, namesAt);
    put64(f, shoff + 128 + 0x20, names.size());
    return f;
}

std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// --- the host the tests judge against: this Switch build's services ------------------------------
// Ids and versions are read from the SDK's own headers (sdk/include/mods/svc/*.h): the macros each
// service module registers with. A header that changes a version changes what is judged here.
struct SdkService {
    std::string id;
    uint16_t major = 0, minor = 0;
};
std::map<std::string, SdkService> g_sdk;  // by id

void read_sdk(const std::filesystem::path& include) {
    const std::regex idRe(R"re(#define\s+(\w+)_SERVICE_ID\s+(?:DUSKLIGHT_SERVICE_ID_PREFIX\s+"([a-z_0-9]+)"|"([a-z_0-9.]+)"))re");
    const std::regex verRe(R"re(#define\s+(\w+)_SERVICE_(MAJOR|MINOR)\s+(\d+)u?)re");
    std::map<std::string, std::string> idOf;
    std::map<std::string, std::pair<int, int>> versions;
    for (const auto& entry : std::filesystem::directory_iterator(include / "mods" / "svc")) {
        if (entry.path().extension() != ".h") {
            continue;
        }
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            std::smatch m;
            if (std::regex_search(line, m, idRe)) {
                idOf[m[1]] = m[2].matched ? kPrefix + m[2].str() : m[3].str();
            } else if (std::regex_search(line, m, verRe)) {
                (m[2] == "MAJOR" ? versions[m[1]].first : versions[m[1]].second) = std::stoi(m[3]);
            }
        }
    }
    for (const auto& [name, id] : idOf) {
        if (versions.contains(name)) {
            g_sdk[id] = {id, static_cast<uint16_t>(versions[name].first), static_cast<uint16_t>(versions[name].second)};
        }
    }
    // The API's own: the service id for the host service is in mods/api.h.
}

SdkService sdk(const std::string& suffix) {
    const auto it = g_sdk.find(kPrefix + suffix);
    if (it == g_sdk.end()) {
        std::printf("FAIL: the SDK headers do not define the %s service\n", suffix.c_str());
        ++g_failures;
        return {kPrefix + suffix, 1, 0};
    }
    return it->second;
}

elf::HostServiceLookup switch_host() {
    // Availability is this platform's: the file service exists without a picker; net and websocket
    // need a transport the console does not have (borealis_net_switch.cpp, borealis_ws_switch.cpp).
    return [](std::string_view id) {
        std::vector<elf::HostService> known;
        const auto it = g_sdk.find(std::string(id));
        if (it == g_sdk.end()) {
            return known;
        }
        bool available = true;
        if (id == kPrefix + "file") {
            available = svc::file_service_available(false, false, false, true);
        } else if (id == kPrefix + "net" || id == kPrefix + "websocket") {
            available = false;
        }
        known.push_back({it->second.major, it->second.minor, available});
        return known;
    };
}

void test_file_availability() {
    namespace s = dusk::mods::svc;
    check(s::file_service_available(false, false, false, true), "no picker, platform with I/O: the service exists");
    check(!s::file_service_available(false, false, false, false), "no picker, no platform I/O: it does not");
    check(s::file_service_available(true, false, false, false), "a picker that opens files: it exists");
    check(s::file_service_available(false, true, false, false), "a picker that opens folders: it exists");
    check(s::file_service_available(false, false, true, false), "a picker that exports: it exists");
    check(!s::kFileServiceWithoutPicker, "the host build is not a Switch build");
    static_assert(s::file_service_available(false, false, false, true));
}

void test_scan() {
    const auto meta = concat({header_record(), import_record(kPrefix + "file", 1, 0, false),
        import_record(kPrefix + "message", 1, 1, false), import_record("com.kite.kh2hud.drive", 1, 0, true)});
    const auto scan = elf::scan_service_imports(make_elf(meta));
    check(scan.ok() && scan.imports.size() == 3, "three imports are read, the header skipped");
    if (scan.imports.size() == 3) {
        check(scan.imports[0].id == kPrefix + "file" && scan.imports[0].major == 1 && !scan.imports[0].optional,
            "the required file import");
        check(scan.imports[1].minMinor == 1, "a minimum minor version is read");
        check(scan.imports[2].optional && scan.imports[2].id == "com.kite.kh2hud.drive", "an optional import of another mod");
    }
    const auto none = elf::scan_service_imports(make_elf(header_record(), "other"));
    check(none.ok() && none.imports.empty(), "a library with no modmeta section has no imports");
    check(elf::scan_service_imports(make_elf(header_record(), "modmeta", false)).imports.empty(), "no section headers: no imports, no error");
    check(!elf::scan_service_imports(std::vector<uint8_t>(10, 0)).ok(), "a tiny file is an error");
    std::vector<uint8_t> notElf(128, 0);
    check(!elf::scan_service_imports(notElf).ok(), "a file that is not an ELF is an error");

    auto cut = import_record(kPrefix + "file", 1, 0, false);
    cut.resize(72);
    put16(cut, 0, 72);
    check(!elf::scan_service_imports(make_elf(cut)).ok(), "an import record of the wrong size is an error");
    auto zero = import_record(kPrefix + "file", 1, 0, false);
    put16(zero, 0, 0);
    check(!elf::scan_service_imports(make_elf(zero)).ok(), "a record of size 0 is an error, not a loop");
    auto odd = import_record(kPrefix + "file", 1, 0, false);
    put16(odd, 0, 76);
    check(!elf::scan_service_imports(make_elf(odd)).ok(), "a record size that is not a multiple of 8 is an error");
    auto past = import_record(kPrefix + "file", 1, 0, false);
    put16(past, 0, 4096);
    check(!elf::scan_service_imports(make_elf(past)).ok(), "a record that runs past the section is an error");
    auto unterminated = import_record(std::string(64, 'x'), 1, 0, false);
    check(!elf::scan_service_imports(make_elf(unterminated)).ok(), "an id with no terminator is an error");
    auto empty = import_record("", 1, 0, false);
    check(!elf::scan_service_imports(make_elf(empty)).ok(), "an empty id is an error");

    auto elfBad = make_elf(import_record(kPrefix + "file", 1, 0, false));
    put64(elfBad, 0x28, elfBad.size() + 100);  // section headers past the end
    check(!elf::scan_service_imports(elfBad).ok(), "a section header table past the end is an error");
    // every prefix of a valid file is read without a crash (the sanitizers watch)
    const auto good = make_elf(concat({header_record(), import_record(kPrefix + "file", 1, 0, false)}));
    for (size_t n = 0; n < good.size(); ++n) {
        (void)elf::scan_service_imports(std::span<const uint8_t>(good.data(), n));
    }
}

elf::ServiceImport imp(const std::string& id, uint16_t major, uint16_t minMinor, bool optional) {
    return {id, major, minMinor, optional};
}

void test_check() {
    const auto host = switch_host();
    // 1. everything the file service does is here: a mod that needs it loads
    {
        const std::vector<elf::ServiceImport> imports = {imp(kPrefix + "file", 1, 0, false), imp(kPrefix + "texture", 1, 0, false)};
        const auto problems = elf::check_services(imports, host);
        check(problems.empty(), "the file service is offered: Randomizer-like imports are satisfied");
    }
    // 2. a required service that this platform cannot offer blocks, with the registry's words
    {
        const std::vector<elf::ServiceImport> imports = {imp(kPrefix + "websocket", 1, 0, false)};
        const auto problems = elf::check_services(imports, host);
        check(problems.size() == 1 && problems[0].blocking && problems[0].kind == elf::ServiceProblemKind::Unavailable,
            "a required, unavailable service blocks");
        check(!problems.empty() && problems[0].message == "Mod requires a service that is unavailable (missing: " + kPrefix + "websocket)",
            "with the message ModLoader::resolve_service_imports gives");
        check(elf::first_blocking(problems) == problems[0].message, "first_blocking says it");
    }
    // 3. the same service, optional: listed, not blocking
    {
        const std::vector<elf::ServiceImport> imports = {imp(kPrefix + "websocket", 1, 0, true), imp(kPrefix + "net", 1, 0, true)};
        const auto problems = elf::check_services(imports, host);
        check(problems.size() == 2 && !problems[0].blocking && !problems[1].blocking, "optional unavailable services do not block");
        check(elf::first_blocking(problems).empty(), "so first_blocking is empty");
    }
    // 4. versions
    {
        const std::vector<elf::ServiceImport> tooNewMinor = {imp(kPrefix + "gfx", sdk("gfx").major, static_cast<uint16_t>(sdk("gfx").minor + 1), false)};
        auto p = elf::check_services(tooNewMinor, host);
        check(p.size() == 1 && p[0].blocking && p[0].kind == elf::ServiceProblemKind::NewerHost &&
                  p[0].message.starts_with("Mod requires a newer Dusklight version"),
            "a minor version newer than the build's blocks");
        const std::vector<elf::ServiceImport> exact = {imp(kPrefix + "gfx", sdk("gfx").major, sdk("gfx").minor, false)};
        check(elf::check_services(exact, host).empty(), "the build's own minor version is fine");
        const std::vector<elf::ServiceImport> older = {imp(kPrefix + "gfx", sdk("gfx").major, 0, false)};
        check(elf::check_services(older, host).empty(), "an older minor version is fine");
        const std::vector<elf::ServiceImport> newMajor = {imp(kPrefix + "game", static_cast<uint16_t>(sdk("game").major + 1), 0, false)};
        p = elf::check_services(newMajor, host);
        check(p.size() == 1 && p[0].blocking && p[0].kind == elf::ServiceProblemKind::NewerHost, "a major version newer than the build's blocks");
        const std::vector<elf::ServiceImport> oldMajor = {imp(kPrefix + "game", static_cast<uint16_t>(sdk("game").major - 1), 0, false)};
        p = elf::check_services(oldMajor, host);
        check(p.size() == 1 && p[0].blocking && p[0].kind == elf::ServiceProblemKind::OlderMod &&
                  p[0].message.starts_with("Mod must be updated for the current Dusklight version"),
            "a mod built for an older major blocks and is told to update");
        const std::vector<elf::ServiceImport> optionalNewer = {imp(kPrefix + "gfx", sdk("gfx").major, static_cast<uint16_t>(sdk("gfx").minor + 1), true)};
        p = elf::check_services(optionalNewer, host);
        check(p.size() == 1 && !p[0].blocking, "an optional import with a too-new minor does not block");
    }
    // 5. unknown service of the game's namespace
    {
        const std::vector<elf::ServiceImport> imports = {imp(kPrefix + "does_not_exist", 1, 0, false)};
        const auto p = elf::check_services(imports, host);
        check(p.size() == 1 && p[0].blocking && p[0].kind == elf::ServiceProblemKind::Unknown, "a game service that does not exist blocks");
    }
    // 6. another mod's service is not judged here, optional or not
    {
        const std::vector<elf::ServiceImport> imports = {imp("com.kite.kh2hud.drive", 1, 0, false), imp("org.example.thing", 2, 3, true)};
        check(elf::check_services(imports, host).empty(), "services of other mods are left to the loader");
    }
    // 7. several problems: the first blocking one, and the count
    {
        const std::vector<elf::ServiceImport> imports = {imp(kPrefix + "websocket", 1, 0, true), imp(kPrefix + "net", 1, 0, false),
            imp(kPrefix + "websocket", 1, 0, false)};
        const auto p = elf::check_services(imports, host);
        const std::string line = elf::first_blocking(p);
        check(line.find("missing: " + kPrefix + "net") != std::string::npos && line.find("and 1 more") != std::string::npos,
            "the first blocking problem is named, with the number of others");
    }
}

void test_report() {
    const auto host = switch_host();
    const std::string base =
        "{\"schema\":\"dusklight-aleks-native-mod-report\",\"schema_version\":1,\"file\":\"x\",\"mode\":\"dry_run\","
        "\"ok\":true,\"error\":\"none\",\"message\":\"\",\"image\":{\"bytes\":1}}";
    const std::vector<elf::ServiceImport> good = {imp(kPrefix + "file", 1, 0, false), imp("com.a.b", 1, 0, false)};
    const auto ok = elf::annotate_report(base, good, elf::check_services(good, host));
    check(ok.find("\"ok\":true") != std::string::npos && ok.find("\"services\":[{\"id\":\"" + kPrefix + "file\"") != std::string::npos &&
              ok.find("\"status\":\"ok\"") != std::string::npos && ok.find("com.a.b") == std::string::npos,
        "a satisfied import is listed as ok; another mod's service is not listed");
    check(ok.back() == '}' && ok.find("\"image\":{\"bytes\":1},\"services\"") != std::string::npos, "the array is added inside the object");

    const std::vector<elf::ServiceImport> bad = {imp(kPrefix + "websocket", 1, 0, false), imp(kPrefix + "net", 1, 0, true)};
    const auto blocked = elf::annotate_report(base, bad, elf::check_services(bad, host));
    check(blocked.find("\"ok\":false,\"error\":\"service_unavailable\",\"message\":\"Mod requires a service that is unavailable (missing: " +
                       kPrefix + "websocket)\"") != std::string::npos,
        "a blocking problem turns the report into ok:false with the message");
    check(blocked.find("\"status\":\"unavailable\"") != std::string::npos, "each problem names its status");

    const std::vector<elf::ServiceImport> optionalOnly = {imp(kPrefix + "websocket", 1, 0, true)};
    const auto soft = elf::annotate_report(base, optionalOnly, elf::check_services(optionalOnly, host));
    check(soft.find("\"ok\":true") != std::string::npos && soft.find("\"status\":\"unavailable\"") != std::string::npos,
        "an optional problem is reported but the report stays ok");
    check(elf::annotate_report("no braces here", good, {}) == "no braces here", "a malformed report is returned untouched");
}

void test_randomizer() {
    const char* path = std::getenv("H37_RANDOMIZER_SO");
    if (path == nullptr || *path == '\0') {
        std::printf("note: H37_RANDOMIZER_SO not set; Randomizer's real imports not read\n");
        return;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(!file.empty(), "Randomizer's library is readable");
    const auto scan = elf::scan_service_imports(file);
    check(scan.ok(), "Randomizer's metadata is read");
    bool needsFile = false;
    for (const auto& i : scan.imports) {
        needsFile |= i.id == kPrefix + "file" && !i.optional;
    }
    check(needsFile, "Randomizer imports the file service, and as a required one (the c3 failure)");
    const auto problems = elf::check_services(scan.imports, switch_host());
    for (const auto& p : problems) {
        std::printf("  Randomizer: %s%s\n", p.message.c_str(), p.blocking ? "" : " (optional)");
    }
    check(elf::first_blocking(problems).empty(), "this Switch build satisfies every service Randomizer requires");
    std::printf("Randomizer: %zu service import(s), %zu problem(s)\n", scan.imports.size(), problems.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: mod_services_test SDK_INCLUDE_DIR\n");
        return 2;
    }
    read_sdk(argv[1]);
    check(g_sdk.size() >= 20, "the SDK headers define the game's services");
    test_file_availability();
    test_scan();
    test_check();
    test_report();
    test_randomizer();
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("mod_services: all checks passed\n");
    return 0;
}
