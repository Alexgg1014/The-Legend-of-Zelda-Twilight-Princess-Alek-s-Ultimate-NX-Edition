// TLS-lab: the loader's TLS paths on the host (x86-64, ASan + UBSan + _GLIBCXX_ASSERTIONS), no AArch64
// code run. Built and run by test-elf-tls.sh.
//
//   elf_tls_host_test DIR [CATALOG_LIB...]
//
// With a recording TlsProvider and a heap "memory backend" it maps tls_desc.so, tls_trad.so and the
// catalog libraries and checks every TLS relocation's written words against the provider's answers
// (TLSDESC pair, DTPMOD64 id, DTPREL64 offset), that thread-local symbols are not exported, and that
// a module's template is registered once with its PT_TLS; then targeted corruptions of PT_TLS and
// of the relocations, and deterministic mutations of tls_desc.so through parse, plan and map.
#include "dusk/mods/loader/elf_loader.hpp"
#include "dusk/mods/loader/elf_runtime_policy.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace dusk::mods::elf;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

struct HeapMemory : MemoryBackend {
    std::vector<uint8_t> bytes;
    uint64_t base = 0x7000'0000'0000ull;
    bool reserve(uint64_t size, uint64_t, uint8_t*& staging, uint64_t& runtimeBase, std::string&) override {
        bytes.assign(size, 0xCC);
        staging = bytes.data();
        runtimeBase = base;
        return true;
    }
    bool publish(const std::vector<Protection>&, std::string&) override { return true; }
    uint64_t word(uint64_t vaddrFromBase) const {
        uint64_t v = 0;
        std::memcpy(&v, bytes.data() + vaddrFromBase, 8);
        return v;
    }
};

// Answers are recognisable: a descriptor is {0xD0000000 + module, 0xA0000000 + offset}, an import's
// {0xE0000000, accessor + addend}, an accessor module 0x8000'0000'0000'0000 | accessor.
struct RecordingTls : TlsProvider {
    std::vector<TlsTemplate> templates;
    std::vector<uint64_t> images;
    uint64_t register_module(uint64_t image, const TlsTemplate& t, std::string&) override {
        templates.push_back(t);
        images.push_back(image);
        return templates.size();
    }
    std::pair<uint64_t, uint64_t> descriptor(uint64_t module, uint64_t offset) override {
        return {0xD0000000ull + module, 0xA0000000ull + offset};
    }
    std::pair<uint64_t, uint64_t> import_descriptor(uint64_t accessor, int64_t addend) override {
        return {0xE0000000ull, accessor + static_cast<uint64_t>(addend)};
    }
    uint64_t import_module(uint64_t accessor) override { return 0x8000'0000'0000'0000ull | accessor; }
};

constexpr uint64_t kAccessor = 0x5150;

Resolver everything() {
    // The TLS runtime's shims (elf_tls.cpp, AArch64 only) by name: the policy would otherwise deny
    // __tls_get_addr and __cxa_thread_atexit_impl.
    auto table = shims();
    for (const char* name : {"__tls_get_addr", "__cxa_thread_atexit", "__cxa_thread_atexit_impl"}) {
        table.push_back({name, 0x7777, "TLS runtime (fake address on the host)"});
    }
    return make_resolver(PackageAbi::Linux, [](std::string_view) -> std::optional<uint64_t> { return 0x1234; },
        table, image_runtime(), [](std::string_view) -> std::optional<uint64_t> { return kAccessor; });
}

// Maps `file` with TLS on and checks each TLS relocation's words. Returns false if it did not map.
bool check_words(const std::string& label, const std::vector<uint8_t>& file, bool requireTls) {
    const auto parsed = parse(file);
    CHECK(parsed.image.has_value());
    if (!parsed.image) return false;
    const Image& im = *parsed.image;
    // Make the policy's denials irrelevant here: every problem other than a denial must be absent.
    const auto p = plan(im, everything(), make_needed_policy(PackageAbi::Linux), true);
    for (const auto& pr : p.problems) {
        CHECK(pr.error == Error::DeniedSymbol || pr.error == Error::NeededRejected);
    }
    if (!p.ok()) {
        std::printf("  %s: planned; %zu denial(s) keep it from mapping here\n", label.c_str(), p.problems.size());
        return false;
    }
    HeapMemory memory;
    RecordingTls tls;
    const auto r = map(im, p, memory, &tls);
    CHECK(r.loaded.has_value());
    if (!r.loaded) return false;
    CHECK(tls.templates.size() == (im.tls ? 1u : 0u));
    if (im.tls) {
        CHECK(tls.templates[0].memsz == im.tls->memsz && tls.templates[0].filesz == im.tls->filesz);
        CHECK(tls.images[0] == memory.base + (im.tls->vaddr - im.base));
        CHECK(r.loaded->tlsModule == 1);
    }
    std::map<uint32_t, uint64_t> seen;
    for (const auto& rel : im.relocations) {
        const uint64_t at = rel.offset - im.base;
        const Symbol* sym = rel.symbol ? &im.symbols[rel.symbol] : nullptr;
        const bool import = sym && !sym->defined();
        const uint64_t offset = (sym && !import ? sym->value : 0) + static_cast<uint64_t>(rel.addend);
        switch (rel.type) {
        case kRelTlsDesc:
            ++seen[rel.type];
            if (import) {
                CHECK(memory.word(at) == 0xE0000000ull && memory.word(at + 8) == kAccessor + static_cast<uint64_t>(rel.addend));
            } else {
                CHECK(memory.word(at) == 0xD0000001ull && memory.word(at + 8) == 0xA0000000ull + offset);
                CHECK(offset < im.tls->memsz);
            }
            break;
        case kRelTlsDtpMod64:
            ++seen[rel.type];
            CHECK(memory.word(at) == (import ? (0x8000'0000'0000'0000ull | kAccessor) : 1u));
            break;
        case kRelTlsDtpRel64:
            ++seen[rel.type];
            CHECK(memory.word(at) == offset);
            break;
        default:
            break;
        }
    }
    for (const auto& [name, address] : r.loaded->exports) {
        for (const auto& s : im.symbols) {
            CHECK(!(s.name == name && s.type == 6));  // no STT_TLS symbol among the exports
        }
        (void)address;
    }
    CHECK(!requireTls || !seen.empty());
    std::printf("  %s: mapped; TLSDESC %lu, DTPMOD64 %lu, DTPREL64 %lu, every word as the provider said\n",
        label.c_str(), (unsigned long)seen[kRelTlsDesc], (unsigned long)seen[kRelTlsDtpMod64],
        (unsigned long)seen[kRelTlsDtpRel64]);
    return true;
}

template <class T>
void put(std::vector<uint8_t>& b, uint64_t at, T v) {
    std::memcpy(b.data() + at, &v, sizeof(T));
}
template <class T>
T get(const std::vector<uint8_t>& b, uint64_t at) {
    T v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

Error first_error(const std::vector<uint8_t>& b, bool tls = true) {
    const auto parsed = parse(b);
    if (!parsed.image) return parsed.diagnostic.error;
    const auto p = plan(*parsed.image, everything(), make_needed_policy(PackageAbi::Linux), tls);
    return p.ok() ? Error::None : p.problems.front().error;
}

void expect(const char* what, Error got, Error want) {
    if (got != want) {
        std::fprintf(stderr, "corruption '%s': got %s, want %s\n", what, error_code(got), error_code(want));
        ++g_failures;
    }
}

void corruptions(const std::vector<uint8_t>& original) {
    const uint64_t phoff = get<uint64_t>(original, 32);
    const uint16_t phnum = get<uint16_t>(original, 56);
    uint64_t tlsHeader = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        if (get<uint32_t>(original, phoff + 56ull * i) == 7) tlsHeader = phoff + 56ull * i;
    }
    CHECK(tlsHeader != 0);
    if (tlsHeader == 0) return;
    expect("original", first_error(original), Error::None);
    expect("original, TLS off", first_error(original, false), Error::TlsUnsupported);
    auto b = original;
    put<uint64_t>(b, tlsHeader + 40, 1ull << 30);  // memsz 1 GiB
    expect("PT_TLS memsz huge", first_error(b), Error::BadProgramHeaders);
    b = original; put<uint64_t>(b, tlsHeader + 32, get<uint64_t>(original, tlsHeader + 40) + 1);
    expect("PT_TLS filesz > memsz", first_error(b), Error::BadProgramHeaders);
    b = original; put<uint64_t>(b, tlsHeader + 48, 3);
    expect("PT_TLS align 3", first_error(b), Error::BadProgramHeaders);
    b = original; put<uint64_t>(b, tlsHeader + 48, 1ull << 20);
    expect("PT_TLS align 1 MiB", first_error(b), Error::BadProgramHeaders);
    b = original; put<uint64_t>(b, tlsHeader + 16, 0x7FFF'0000'0000ull);  // image outside every PT_LOAD
    expect("PT_TLS image outside", first_error(b), Error::BadProgramHeaders);
    b = original; put<uint64_t>(b, tlsHeader + 40, 4); put<uint64_t>(b, tlsHeader + 32, 4);  // shrink: symbols fall outside
    expect("TLS symbol outside PT_TLS", first_error(b), Error::BadSymbolTable);

    // Relocations: the first TLSDESC in DT_RELA or DT_JMPREL (the linker puts them in .rela.plt).
    const auto parsed = parse(original);
    const Image& im = *parsed.image;
    uint64_t desc = 0;
    for (uint16_t i = 0; i < phnum && desc == 0; ++i) {
        const uint64_t h = phoff + 56ull * i;
        if (get<uint32_t>(original, h) != 2) continue;
        const uint64_t off = get<uint64_t>(original, h + 8), size = get<uint64_t>(original, h + 32);
        for (const auto& [tableTag, sizeTag] : {std::pair<int64_t, int64_t>{7, 8}, {23, 2}}) {
            uint64_t vaddr = 0, bytes = 0, file = 0;
            for (uint64_t at = off; at + 16 <= off + size; at += 16) {
                const int64_t tag = get<int64_t>(original, at);
                if (tag == tableTag) vaddr = get<uint64_t>(original, at + 8);
                if (tag == sizeTag) bytes = get<uint64_t>(original, at + 8);
            }
            for (const auto& s : im.loads) {
                if (vaddr >= s.vaddr && vaddr < s.vaddr + s.filesz) file = s.offset + vaddr - s.vaddr;
            }
            for (uint64_t at = file; file && at < file + bytes && desc == 0; at += 24) {
                if ((get<uint64_t>(original, at + 8) & 0xFFFFFFFF) == kRelTlsDesc) desc = at;
            }
        }
    }
    CHECK(desc != 0);
    if (desc == 0) return;
    const uint64_t info = get<uint64_t>(original, desc + 8);
    b = original; put<uint64_t>(b, desc + 8, (info & ~0xFFFFFFFFull) | kRelTlsTpRel64);
    expect("TPREL64 with TLS on", first_error(b), Error::TlsUnsupported);
    b = original; put<uint64_t>(b, desc + 8, (info & ~0xFFFFFFFFull) | kRelTlsTpRel64);
    expect("TPREL64 with TLS off", first_error(b, false), Error::TlsUnsupported);
    // TLSDESC against a non-TLS symbol: mod_update is a function the module defines.
    uint32_t function = 0;
    for (uint32_t i = 0; i < im.symbols.size(); ++i) {
        if (im.symbols[i].name == "mod_update") function = i;
    }
    CHECK(function != 0);
    b = original; put<uint64_t>(b, desc + 8, (uint64_t{function} << 32) | kRelTlsDesc);
    expect("TLSDESC against a function", first_error(b), Error::BadRelocationTable);
    // GLOB_DAT against a thread-local.
    uint32_t tlsSym = 0;
    for (uint32_t i = 0; i < im.symbols.size(); ++i) {
        if (im.symbols[i].type == 6 && im.symbols[i].defined()) tlsSym = i;
    }
    CHECK(tlsSym != 0);
    b = original; put<uint64_t>(b, desc + 8, (uint64_t{tlsSym} << 32) | kRelGlobDat);
    expect("GLOB_DAT against a thread-local", first_error(b), Error::BadRelocationTable);
    // A TLSDESC whose pair would run into a read-only page: the first byte of the image is code.
    b = original; put<uint64_t>(b, desc, im.loads.front().vaddr + 16);
    expect("TLSDESC into code", first_error(b), Error::TextRelocations);
    std::printf("  targeted corruptions of PT_TLS and TLS relocations: as expected\n");
}

void fuzz(const std::vector<uint8_t>& original) {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    int mapped = 0;
    for (int i = 0; i < 20000; ++i) {
        auto b = original;
        const int edits = 1 + static_cast<int>(next() % 4);
        for (int e = 0; e < edits; ++e) {
            // Mostly the headers and the dynamic tables, where TLS lives; sometimes anywhere.
            const uint64_t at = (next() & 3) ? next() % std::min<uint64_t>(b.size(), 0x2000) : next() % b.size();
            b[at] = static_cast<uint8_t>(next());
        }
        const auto parsed = parse(b);
        if (!parsed.image) continue;
        const auto p = plan(*parsed.image, everything(), make_needed_policy(PackageAbi::Linux), (i & 1) == 0);
        if (!p.ok()) continue;
        HeapMemory memory;
        RecordingTls tls;
        if (map(*parsed.image, p, memory, &tls).loaded) ++mapped;
    }
    std::printf("  20000 mutations through parse, plan and map: no fault (%d still mapped)\n", mapped);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s DIR [CATALOG_LIB...]\n", argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string dir = argv[1];
    const auto desc = read_file(dir + "/tls_desc.so");
    CHECK(check_words("tls_desc.so", desc, true));
    CHECK(check_words("tls_trad.so", read_file(dir + "/tls_trad.so"), true));
    for (int i = 2; i < argc; ++i) {
        check_words(argv[i], read_file(argv[i]), true);
    }
    corruptions(desc);
    fuzz(desc);
    if (g_failures) {
        std::fprintf(stderr, "elf_tls_host_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("elf_tls_host_test: all checks passed\n");
    return 0;
}
