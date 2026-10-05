// H37: host test for src/dusk/mods/loader/elf_loader.cpp and elf_runtime_policy.cpp, against
// real AArch64 mod libraries (built by build-h37-test-mods.sh) and damaged copies of them.
//
//   elf_loader_test MODS_DIR [REPORT_DIR]
//
// What is checked, and against what:
//   * every relocation the loader writes, against binutils' own reading of the file
//     (MODS_DIR/<name>.relocs, made with readelf by the build script), not against the
//     loader's parser;
//   * the final page permissions: never writable and executable, RELRO read-only, code RX;
//   * that each targeted corruption is refused with its own error code, and that a corpus of
//     deterministic random mutations never crashes the parser, the planner or the mapper
//     (run under ASan, UBSan and _GLIBCXX_ASSERTIONS by test-elf-loader.sh);
//   * Dry Run allocates no executable memory and calls no code;
//   * the runtime policy: shims first, refusals by name, caveats carried to the report,
//     DT_NEEDED by package ABI.
#include "dusk/mods/loader/elf_loader.hpp"
#include "dusk/mods/loader/elf_runtime_policy.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dusk::mods::elf;

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// A fake game: every name gets a distinct, recognisable address, unless it is in `absent`.
uint64_t fake_address(std::string_view name) {
    uint64_t h = 1469598103934665603ull;
    for (const char c : name) {
        h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    }
    return 0x7000000000ull | (h & 0xFFFFFFF0ull);
}

GameLookup fake_game(std::set<std::string> absent = {}) {
    return [absent = std::move(absent)](std::string_view name) -> std::optional<uint64_t> {
        if (absent.contains(std::string(name))) {
            return std::nullopt;
        }
        return fake_address(name);
    };
}

// Host memory standing in for the console: a staging buffer, a pretend runtime address, and
// a record of what was published. Nothing here is ever executable.
struct RecordingMemory : MemoryBackend {
    std::vector<uint8_t> buffer;
    uint64_t runtime = 0x7100000000ull;
    std::vector<Protection> published;
    int reserves = 0;
    bool reserve(uint64_t size, uint64_t align, uint8_t*& staging, uint64_t& runtimeBase, std::string&) override {
        ++reserves;
        buffer.assign(size, 0xCC);
        staging = buffer.data();
        runtimeBase = runtime;
        CHECK((runtime % align) == 0);
        return true;
    }
    bool publish(const std::vector<Protection>& ranges, std::string&) override {
        published = ranges;
        return true;
    }
};

struct OracleEntry {
    uint64_t offset;
    std::string type;
    std::string symbol;
    int64_t addend;
};

std::vector<OracleEntry> read_oracle(const fs::path& p) {
    std::vector<OracleEntry> out;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string off, type, sym, add;
        s >> off >> type >> sym >> add;
        const bool negative = !add.empty() && add[0] == '-';
        const uint64_t magnitude = std::stoull(negative ? add.substr(1) : add, nullptr, 16);
        out.push_back({std::stoull(off, nullptr, 16), type, sym,
            negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude)});
    }
    return out;
}

uint64_t read64(const std::vector<uint8_t>& b, uint64_t at) {
    uint64_t v = 0;
    std::memcpy(&v, b.data() + at, 8);
    return v;
}

// --- real libraries: parse, plan, map, and check every relocation against readelf -----------

void check_library(const fs::path& dir, const std::string& name, bool expectLoadable) {
    const auto so = dir / (name + ".so");
    if (!fs::exists(so)) {
        std::printf("skip %s (not built)\n", name.c_str());
        return;
    }
    const auto bytes = read_file(so);
    const auto parsed = parse(bytes);
    if (!expectLoadable) {
        // The Randomizer as published: thread-local storage, refused by name.
        CHECK(!parsed.image);
        CHECK(parsed.diagnostic.error == Error::TlsUnsupported);
        std::printf("%-14s refused as expected: %s (%s)\n", name.c_str(), error_code(parsed.diagnostic.error),
            parsed.diagnostic.message.c_str());
        return;
    }
    CHECK(parsed.image.has_value());
    if (!parsed.image) {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), parsed.diagnostic.message.c_str());
        return;
    }
    const Image& image = *parsed.image;

    // Linux compatibility: a fake game that defines every name.
    const auto plan1 = plan(image, make_resolver(PackageAbi::Linux, fake_game()), make_needed_policy(PackageAbi::Linux));
    for (const auto& p : plan1.problems) {
        std::fprintf(stderr, "%s: problem %s: %s\n", name.c_str(), error_code(p.error), p.message.c_str());
    }
    CHECK(plan1.ok());
    // The same file as a Switch package: its glibc DT_NEEDED and versioned imports are refused.
    const auto planSwitch = plan(image, make_resolver(PackageAbi::Switch, fake_game()), make_needed_policy(PackageAbi::Switch));
    CHECK(image.needed.empty() || !planSwitch.ok());

    RecordingMemory memory;
    const auto mapped = map(image, plan1, memory);
    CHECK(mapped.loaded.has_value());
    if (!mapped.loaded) {
        std::fprintf(stderr, "%s: map: %s\n", name.c_str(), mapped.diagnostic.message.c_str());
        return;
    }
    const Loaded& loaded = *mapped.loaded;
    const uint64_t bias = loaded.runtimeBase - image.base;

    // Expected S for each symbol name, from the plan's decisions (the shim table for shims).
    std::map<std::string, uint64_t> sValue;
    for (const auto& s : shims()) {
        sValue[std::string(s.name)] = s.address;
    }
    for (const auto& d : plan1.imports) {
        if (d.source == Source::Module) {
            sValue[std::string(d.name)] = bias + d.address;
        } else if (d.source == Source::WeakNull) {
            sValue[std::string(d.name)] = 0;
        } else if (d.source == Source::Runtime) {
            CHECK(d.address == fake_address(d.name));  // the resolver's answer, untouched
            sValue[std::string(d.name)] = d.address;
        }
    }
    const auto oracle = read_oracle(dir / (name + ".relocs"));
    CHECK(oracle.size() == image.relocations.size());
    std::map<std::string, int> checked;
    for (const auto& r : oracle) {
        const uint64_t at = r.offset - image.base;
        const uint64_t got = read64(memory.buffer, at);
        uint64_t want = 0;
        if (r.type == "R_AARCH64_RELATIVE") {
            want = bias + static_cast<uint64_t>(r.addend);
        } else if (r.type == "R_AARCH64_GLOB_DAT" || r.type == "R_AARCH64_JUMP_SLOT" || r.type == "R_AARCH64_ABS64") {
            const auto it = sValue.find(r.symbol);
            CHECK(it != sValue.end());
            want = (it == sValue.end() ? 0 : it->second) + static_cast<uint64_t>(r.addend);
        } else {
            CHECK(false && "relocation type the oracle did not expect");
        }
        if (got != want) {
            std::fprintf(stderr, "%s: %s at %#" PRIx64 " (%s): wrote %#" PRIx64 ", readelf implies %#" PRIx64 "\n",
                name.c_str(), r.type.c_str(), r.offset, r.symbol.c_str(), got, want);
            ++g_failures;
        }
        ++checked[r.type];
    }

    // Segments: file bytes copied, .bss zero, gaps zero (the buffer started as 0xCC).
    for (const auto& s : image.loads) {
        for (uint64_t i = s.filesz; i < s.memsz; ++i) {
            if (memory.buffer[s.vaddr - image.base + i] != 0) {
                CHECK(false && ".bss not zeroed");
                break;
            }
        }
    }
    // Final permissions: every page covered, never W+X, code RX, RELRO read-only.
    uint64_t covered = 0;
    bool sawRx = false;
    for (const auto& p : memory.published) {
        CHECK(p.offset == covered);
        covered += p.size;
        CHECK(!((p.perm & kPermW) && (p.perm & kPermX)));
        sawRx = sawRx || p.perm == (kPermR | kPermX);
    }
    CHECK(covered == image.size);
    CHECK(sawRx);
    if (image.relro) {
        const uint64_t relroPage = (image.relro->vaddr + 4095) / 4096 * 4096 - image.base;
        const uint64_t relroEnd = (image.relro->vaddr + image.relro->memsz) / 4096 * 4096 - image.base;
        for (const auto& p : memory.published) {
            if (p.offset >= relroPage && p.offset + p.size <= relroEnd) {
                CHECK(!(p.perm & kPermW));
            }
        }
    }
    // The five exports, the constructors (inside code), .eh_frame.
    for (const auto e : kModExports) {
        CHECK(loaded.find(e) != 0);
    }
    for (const uint64_t c : loaded.constructors) {
        CHECK(c >= loaded.runtimeBase && c < loaded.runtimeBase + loaded.size);
    }
    CHECK(loaded.ehFrame != 0);
    std::string counts;
    for (const auto& [t, n] : checked) {
        counts += " " + t.substr(10) + "=" + std::to_string(n);
    }
    std::printf("%-14s %zu relocations match readelf (%s), %zu page range(s), %zu constructor(s)\n", name.c_str(),
        oracle.size(), counts.c_str() + 1, memory.published.size(), loaded.constructors.size());
}

// --- missing game symbols, Dry Run ------------------------------------------------------------

void check_missing_and_dry_run(const fs::path& dir) {
    const auto so = dir / "rollandsprint.so";
    if (!fs::exists(so)) {
        return;
    }
    const auto bytes = read_file(so);
    const auto parsed = parse(bytes);
    CHECK(parsed.image.has_value());
    if (!parsed.image) {
        return;
    }
    // A game that has none of the game functions Roll and Sprint calls: each one named.
    const std::set<std::string> absent{"_ZN9daAlink_c12procMoveInitEv", "dComIfGp_getVibration"};
    std::set<std::string> gameNames;
    for (const auto& s : parsed.image->symbols) {
        if (!s.defined() && std::string(s.name).find("daAlink") != std::string::npos) {
            gameNames.insert(std::string(s.name));
        }
    }
    CHECK(!gameNames.empty());
    const auto p = plan(*parsed.image, make_resolver(PackageAbi::Linux, fake_game(gameNames)),
        make_needed_policy(PackageAbi::Linux));
    CHECK(!p.ok());
    bool named = false;
    for (const auto& pr : p.problems) {
        if (pr.error == Error::MissingSymbol) {
            named = pr.message.find(*gameNames.begin()) != std::string::npos;
        }
    }
    CHECK(named);
    // Dry Run never reserves memory: map() refuses a failed plan before touching the backend.
    RecordingMemory memory;
    const auto m = map(*parsed.image, p, memory);
    CHECK(!m.loaded && memory.reserves == 0);
    const std::string json = report_json("rollandsprint.so", "dry_run", parsed, &p);
    CHECK(json.find("\"ok\":false") != std::string::npos);
    CHECK(json.find("\"error\":\"missing_symbol\"") != std::string::npos);
    std::ofstream(dir / "rollandsprint.dryrun.json") << json << "\n";
    std::printf("dry run      rollandsprint without game functions: refused, %zu game import(s) named\n",
        gameNames.size());
}

// --- targeted corruptions: each refused with its own code ---------------------------------------

struct Layout {
    uint64_t phoff = 0;
    uint16_t phnum = 0;
    std::vector<uint64_t> loadHeaders;  // offsets of PT_LOAD headers
    uint64_t dynamicHeader = 0, dynamicOffset = 0, dynamicSize = 0;
    uint64_t stackHeader = 0;
    uint64_t relroHeader = 0;
};

Layout layout_of(const std::vector<uint8_t>& b) {
    Layout l;
    std::memcpy(&l.phoff, b.data() + 32, 8);
    std::memcpy(&l.phnum, b.data() + 56, 2);
    for (uint16_t i = 0; i < l.phnum; ++i) {
        const uint64_t at = l.phoff + 56ull * i;
        uint32_t type = 0;
        std::memcpy(&type, b.data() + at, 4);
        if (type == 1) l.loadHeaders.push_back(at);
        if (type == 2) {
            l.dynamicHeader = at;
            std::memcpy(&l.dynamicOffset, b.data() + at + 8, 8);
            std::memcpy(&l.dynamicSize, b.data() + at + 32, 8);
        }
        if (type == 0x6474E551) l.stackHeader = at;
        if (type == 0x6474E552) l.relroHeader = at;
    }
    return l;
}

template <class T>
void put(std::vector<uint8_t>& b, uint64_t at, T v) {
    std::memcpy(b.data() + at, &v, sizeof(T));
}

// Offset of the dynamic entry with `tag`, or 0.
uint64_t dyn_entry(const std::vector<uint8_t>& b, const Layout& l, int64_t tag) {
    for (uint64_t at = l.dynamicOffset; at + 16 <= l.dynamicOffset + l.dynamicSize; at += 16) {
        int64_t t = 0;
        std::memcpy(&t, b.data() + at, 8);
        if (t == tag) return at;
        if (t == 0) break;
    }
    return 0;
}

Error parse_error(const std::vector<uint8_t>& b) {
    return parse(b).diagnostic.error;
}

Error plan_error(const std::vector<uint8_t>& b, PackageAbi abi = PackageAbi::Linux, GameLookup game = fake_game()) {
    const auto parsed = parse(b);
    if (!parsed.image) return parsed.diagnostic.error;
    const auto p = plan(*parsed.image, make_resolver(abi, std::move(game)), make_needed_policy(abi));
    return p.ok() ? Error::None : p.problems.front().error;
}

void check_corruptions(const fs::path& dir) {
    const auto original = read_file(dir / "loader_lab.so");
    if (original.empty()) return;
    const Layout l = layout_of(original);
    CHECK(l.loadHeaders.size() >= 2 && l.dynamicHeader != 0);
    int cases = 0;
    const auto expect = [&](const char* what, Error got, Error want) {
        ++cases;
        if (got != want) {
            std::fprintf(stderr, "corruption '%s': got %s, want %s\n", what, error_code(got), error_code(want));
            ++g_failures;
        }
    };
    auto b = original;
    CHECK(parse_error(b) == Error::None);

    b = original; b.resize(40);                         expect("header cut short", parse_error(b), Error::Truncated);
    b = original; b[0] = 0x7E;                          expect("bad magic", parse_error(b), Error::BadMagic);
    b = original; b[4] = 1;                             expect("ELFCLASS32", parse_error(b), Error::NotElf64);
    b = original; b[5] = 2;                             expect("big-endian", parse_error(b), Error::NotLittleEndian);
    b = original; put<uint16_t>(b, 18, 62);             expect("x86-64 machine", parse_error(b), Error::WrongMachine);
    b = original; put<uint16_t>(b, 16, 2);              expect("ET_EXEC", parse_error(b), Error::WrongType);
    b = original; put<uint16_t>(b, 54, 32);             expect("e_phentsize", parse_error(b), Error::BadHeader);
    b = original; put<uint64_t>(b, 32, ~0ull - 8);      expect("e_phoff overflow", parse_error(b), Error::BadProgramHeaders);
    b = original; put<uint16_t>(b, 56, 4000);           expect("phnum huge", parse_error(b), Error::TooManySegments);
    const uint64_t h0 = l.loadHeaders[0], h1 = l.loadHeaders[1];
    b = original; put<uint64_t>(b, h1 + 8, b.size() - 16);
                                                        expect("segment past EOF", parse_error(b), Error::SegmentOutOfFile);
    b = original; put<uint64_t>(b, h1 + 8, ~0ull - 100);
                                                        expect("segment offset overflow", parse_error(b), Error::SegmentOutOfFile);
    b = original; { uint64_t m; std::memcpy(&m, b.data() + h1 + 40, 8); put<uint64_t>(b, h1 + 32, m + 4096); }
                                                        expect("filesz > memsz", parse_error(b), Error::FileSizeExceedsMemSize);
    b = original; put<uint64_t>(b, h1 + 48, 3);         expect("align 3", parse_error(b), Error::BadAlignment);
    b = original; put<uint64_t>(b, h1 + 48, 1ull << 30); expect("align 1 GiB", parse_error(b), Error::BadAlignment);
    b = original; { uint64_t v; std::memcpy(&v, b.data() + h0 + 16, 8);
                    uint64_t o; std::memcpy(&o, b.data() + h0 + 8, 8);
                    put<uint64_t>(b, h1 + 16, v); put<uint64_t>(b, h1 + 8, o); }
                                                        expect("overlapping PT_LOADs", parse_error(b), Error::SegmentOverlap);
    b = original; put<uint32_t>(b, h1 + 4, 7);          expect("RWX segment", parse_error(b), Error::WriteExecSegment);
    b = original; put<uint64_t>(b, h1 + 40, ~0ull - 0x100);
                                                        expect("memsz wraps", parse_error(b), Error::SegmentOverflow);
    b = original; put<uint64_t>(b, h1 + 40, 1ull << 40); expect("image too large", parse_error(b), Error::ImageTooLarge);
    if (l.stackHeader) {
        b = original; put<uint32_t>(b, l.stackHeader, 7);  expect("PT_TLS", parse_error(b), Error::TlsUnsupported);
        b = original; put<uint32_t>(b, l.stackHeader, 3);  expect("PT_INTERP", parse_error(b), Error::InterpreterPresent);
    }
    if (l.relroHeader) {  // the fuzz corpus's first find: a wild PT_GNU_RELRO used to hang protections()
        b = original; put<uint64_t>(b, l.relroHeader + 16, 0x6474E55200000000ull);
                                                        expect("PT_GNU_RELRO past the image", parse_error(b), Error::BadProgramHeaders);
        b = original; put<uint64_t>(b, l.relroHeader + 40, ~0ull - 0x10);
                                                        expect("PT_GNU_RELRO memsz wraps", parse_error(b), Error::BadProgramHeaders);
    }
    b = original; put<uint32_t>(b, l.dynamicHeader, 4); expect("no PT_DYNAMIC", parse_error(b), Error::NoDynamic);
    b = original; put<uint64_t>(b, l.dynamicHeader + 8, b.size());
                                                        expect("PT_DYNAMIC outside file", parse_error(b), Error::BadDynamic);
    if (const uint64_t e = dyn_entry(original, l, 21)) {  // DT_DEBUG -> DT_TEXTREL
        b = original; put<int64_t>(b, e, 22);           expect("DT_TEXTREL", parse_error(b), Error::TextRelocations);
        b = original; put<int64_t>(b, e, 36);           expect("DT_RELR", parse_error(b), Error::RelrUnsupported);
        b = original; put<int64_t>(b, e, 17);           expect("DT_REL", parse_error(b), Error::BadDynamic);
    }
    if (const uint64_t e = dyn_entry(original, l, 10)) {  // DT_STRSZ
        b = original; put<uint64_t>(b, e + 8, 1ull << 40); expect("DT_STRSZ huge", parse_error(b), Error::BadStringTable);
    }
    if (const uint64_t e = dyn_entry(original, l, 0x6FFFFEF5)) {  // DT_GNU_HASH
        b = original; put<uint64_t>(b, e + 8, 1ull << 40); expect("GNU hash outside", parse_error(b), Error::BadSymbolTable);
    }
    if (const uint64_t e = dyn_entry(original, l, 8)) {  // DT_RELASZ
        b = original; put<uint64_t>(b, e + 8, 25);      expect("DT_RELASZ not x24", parse_error(b), Error::BadRelocationTable);
    }
    if (const uint64_t e = dyn_entry(original, l, 27)) {  // DT_INIT_ARRAYSZ
        b = original; put<uint64_t>(b, e + 8, 7);       expect("init_array size 7", parse_error(b), Error::BadInitArray);
    }

    // Through the relocation table: a TLSDESC, a symbol index out of range, a target in code.
    const auto parsed = parse(original);
    const uint64_t relaVaddr = [&] { const uint64_t e = dyn_entry(original, l, 7); uint64_t v = 0; std::memcpy(&v, original.data() + e + 8, 8); return v; }();
    uint64_t relaFile = 0;
    for (const auto& s : parsed.image->loads) {
        if (relaVaddr >= s.vaddr && relaVaddr < s.vaddr + s.filesz) relaFile = s.offset + (relaVaddr - s.vaddr);
    }
    CHECK(relaFile != 0);
    b = original; put<uint64_t>(b, relaFile + 8, (uint64_t{0} << 32) | 1031u);
                                                        expect("TLSDESC relocation", plan_error(b), Error::UnsupportedRelocation);
    b = original; put<uint64_t>(b, relaFile + 8, (uint64_t{0} << 32) | 1032u);
                                                        expect("IRELATIVE relocation", parse_error(b), Error::IfuncUnsupported);
    b = original; put<uint64_t>(b, relaFile + 8, (uint64_t{0xFFFFFF} << 32) | 1025u);
                                                        expect("symbol index out of range", parse_error(b), Error::BadRelocationTable);
    b = original; put<uint64_t>(b, relaFile, parsed.image->loads.front().vaddr + 16);
                                                        expect("relocation into code", plan_error(b), Error::TextRelocations);
    b = original; put<uint64_t>(b, relaFile, 1ull << 40);
                                                        expect("relocation outside image", plan_error(b), Error::RelocationOutOfImage);
    // Policy: a Switch package may not name libc.so.6; a missing import is named; a refused one too.
    expect("DT_NEEDED as a Switch package", plan_error(original, PackageAbi::Switch), Error::NeededRejected);
    expect("missing import", plan_error(original, PackageAbi::Linux, fake_game({"memcmp", "__cxa_throw"})),
        Error::MissingSymbol);
    // A renamed export: the string "mod_update" in .dynstr becomes "mod_updatX".
    b = original;
    {
        const std::string needle = "mod_update";
        auto it = std::search(b.begin(), b.end(), needle.begin(), needle.end());
        CHECK(it != b.end());
        if (it != b.end()) *(it + 9) = 'X';
    }
    expect("missing mod_update export", plan_error(b), Error::MissingExport);
    std::printf("corruptions  %d targeted cases, each refused with its own code\n", cases);
}

// --- a corpus of deterministic random mutations: nothing may crash ------------------------------

void fuzz(const fs::path& dir) {
    std::mt19937_64 rng(0x48333744u);  // fixed: the corpus is the same every run
    uint64_t runs = 0, parsedOk = 0, planned = 0, mapped = 0;
    std::map<Error, uint64_t> seen;
    for (const char* name : {"loader_lab", "template_mod", "rollandsprint"}) {
        const auto original = read_file(dir / (std::string(name) + ".so"));
        if (original.empty()) continue;
        const Layout l = layout_of(original);
        for (int i = 0; i < 4000; ++i) {
            auto b = original;
            const int kind = static_cast<int>(rng() % 6);
            if (kind == 0) {
                b.resize(rng() % b.size());  // truncation anywhere
            } else if (kind == 1) {  // flips in the headers
                for (int k = 0; k < 4; ++k) b[rng() % std::min<size_t>(b.size(), 64 + 56ull * l.phnum)] ^= static_cast<uint8_t>(1u << (rng() % 8));
            } else if (kind == 2) {  // flips in the dynamic section
                for (int k = 0; k < 4; ++k) b[l.dynamicOffset + rng() % l.dynamicSize] ^= static_cast<uint8_t>(rng());
            } else if (kind == 3) {  // a random 64-bit word in a program header
                put<uint64_t>(b, l.phoff + (rng() % (56ull * l.phnum)) / 8 * 8, rng());
            } else if (kind == 4) {  // a random dynamic value
                put<uint64_t>(b, l.dynamicOffset + (rng() % (l.dynamicSize / 16)) * 16 + 8, rng() >> (rng() % 64));
            } else {  // flips anywhere
                for (int k = 0; k < 16; ++k) b[rng() % b.size()] ^= static_cast<uint8_t>(rng());
            }
            ++runs;
            if (std::getenv("H37_FUZZ_TRACE")) std::fprintf(stderr, "%s %d kind %d\n", name, i, kind);  // to find a hang
            Limits limits;
            limits.maxImageBytes = 8ull << 20;  // keep host allocations small
            const auto parsed = parse(b, limits);
            ++seen[parsed.diagnostic.error];
            if (!parsed.image) continue;
            ++parsedOk;
            const auto p = plan(*parsed.image, make_resolver(PackageAbi::Linux, fake_game()), make_needed_policy(PackageAbi::Linux));
            ++planned;
            (void)report_json(name, "dry_run", parsed, &p);
            if (!p.ok()) continue;
            RecordingMemory memory;
            const auto m = map(*parsed.image, p, memory);
            if (m.loaded) {
                ++mapped;
                for (const auto& pr : memory.published) {
                    CHECK(!((pr.perm & kPermW) && (pr.perm & kPermX)));
                }
            }
        }
    }
    std::printf("mutations    %" PRIu64 " runs, %" PRIu64 " still parsed, %" PRIu64 " planned, %" PRIu64
                " mapped, %zu distinct refusals, no crash\n", runs, parsedOk, planned, mapped, seen.size());
}

// --- the runtime policy -----------------------------------------------------------------------

void check_policy() {
    const auto linux = make_resolver(PackageAbi::Linux, fake_game({"absent_function"}));
    std::string why;
    const auto r1 = linux({"__cxa_finalize", "GLIBC_2.17", true, true}, why);
    CHECK(r1 && r1->source == Source::Shim && why.find("never unloaded") != std::string::npos);
    const auto r2 = linux({"_IO_putc", "GLIBC_2.17", true, false}, why);
    CHECK(r2 && r2->source == Source::Denied);
    const auto r3 = linux({"pthread_mutex_lock", "GLIBC_2.17", true, false}, why);
    CHECK(r3 && r3->source == Source::Runtime && why.find("recursive") != std::string::npos);
    const auto r4 = linux({"memcpy", "GLIBC_2.17", true, false}, why);
    CHECK(r4 && r4->source == Source::Runtime && r4->address == fake_address("memcpy"));
    const auto r5 = linux({"absent_function", "", true, false}, why);
    CHECK(!r5);
    const auto sw = make_resolver(PackageAbi::Switch, fake_game());
    const auto r6 = sw({"memcpy", "GLIBC_2.17", true, false}, why);
    CHECK(r6 && r6->source == Source::Denied);  // a Switch package has no glibc versions
    const auto r7 = sw({"memcpy", "", true, false}, why);
    CHECK(r7 && r7->source == Source::Runtime);
    const auto lp = make_needed_policy(PackageAbi::Linux);
    const auto sp = make_needed_policy(PackageAbi::Switch);
    CHECK(lp("libstdc++.so.6", why) && !lp("libSDL3.so.0", why) && !sp("libc.so.6", why));
    CHECK(denied("_dl_find_object").has_value() && !denied("printf").has_value());
    std::printf("policy       shims, refusals, caveats and DT_NEEDED by package ABI as specified\n");
}

// Dry Run reports of every library, as each package ABI, against a game that defines every
// name: what test-elf-loader.sh compares with elf_audit.py's verdicts (two independent readers
// of the same files must agree on what can load).
void write_reports(const fs::path& dir, const fs::path& out) {
    fs::create_directories(out);
    int written = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".so") continue;
        const auto bytes = read_file(entry.path());
        const auto parsed = parse(bytes);
        for (const auto abi : {PackageAbi::Linux, PackageAbi::Switch}) {
            const char* tag = abi == PackageAbi::Linux ? "linux" : "switch";
            std::optional<Plan> p;
            if (parsed.image) p = plan(*parsed.image, make_resolver(abi, fake_game()), make_needed_policy(abi));
            std::ofstream(out / (entry.path().stem().string() + "." + tag + ".json"))
                << report_json(entry.path().filename().string(), "dry_run", parsed, p ? &*p : nullptr) << "\n";
            ++written;
        }
    }
    std::printf("reports      %d Dry Run report(s) in %s\n", written, out.string().c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: %s MODS_DIR [REPORT_DIR]\n", argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const fs::path dir = argv[1];
    check_policy();
    for (const char* name : {"loader_lab", "template_mod", "rollandsprint", "cosmetics", "luau_runtime"}) {
        check_library(dir, name, true);
    }
    // Randomizer as build-h37-test-mods.sh builds it, with platforms/switch/patches/randomizer
    // applied: no thread-local storage left, so it loads like the others. The Randomizer as
    // published (one thread_local) is refused for its PT_TLS: if that is what is here, expect it.
    {
        const auto bytes = read_file(dir / "randomizer.so");
        const bool published = !bytes.empty() && parse(bytes).diagnostic.error == Error::TlsUnsupported;
        check_library(dir, "randomizer", !published);
    }
    check_missing_and_dry_run(dir);
    check_corruptions(dir);
    fuzz(dir);
    if (argc == 3) write_reports(dir, argv[2]);
    if (g_failures) {
        std::fprintf(stderr, "elf_loader_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("elf_loader_test: all checks passed\n");
    return 0;
}
