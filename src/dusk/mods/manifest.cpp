#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "manifest.hpp"
#include "abi_tags.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <zstd.h>

#include <borealis/log.hpp>

#if defined(__SWITCH__) && DUSK_CODE_MODS
#include "dusk/switch_builtin_mods.hpp"
#endif
#if defined(__SWITCH__)
#include <cstdio>
// libnx's crt0: the first byte of the image, where the NRO header lives.
extern "C" char _start[];
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#elif defined(__linux__)
#include <elf.h>
#include <link.h>
#endif

namespace dusk::mods::manifest {
namespace {

constexpr borealis::Log Log{"dusk::mods::manifest"};

constexpr char kMagic[8] = {'S', 'Y', 'M', 'G', 'E', 'N', '\0', '\0'};
constexpr uint32_t kVersion = 2;

enum class Compression : uint32_t {
    None = 0,
    Zstd = 1,
};

// Mirrors the symgen manifest writer.
struct Header {
    char magic[8];
    uint32_t version;
    uint32_t compression;
    uint64_t uncompressedLen;
    uint64_t compressedLen;
    uint32_t buildIdLen;
    uint8_t buildId[32];
    uint32_t entryCount;
};
static_assert(sizeof(Header) == 72);

struct Entry {
    uint64_t hash;
    uint64_t rva;
    uint32_t nameOff;
    HookSymbolFlags flags;
};
static_assert(sizeof(Entry) == 24);

/*
 * `symgen manifest --embed` appends the symbol manifest to the linked executable as an added
 * section and patches this descriptor with its location.
 */
struct SymdbDescriptor {
    volatile uint64_t magic;
    volatile uint64_t rva;
    volatile uint64_t size;
};
constexpr uint64_t kDescriptorMagic = 0x52444842444d5953ull;  // "SYMDBHDR"
#if defined(_WIN32)
#pragma section(".symdbh", read)
__declspec(allocate(".symdbh"))
#if defined(__clang__)
__attribute__((used))
#endif
constinit SymdbDescriptor s_symdbDescriptor{kDescriptorMagic, 0, 0};
#elif defined(__APPLE__)
__attribute__((section("__DATA,__symdbh"), used)) constinit SymdbDescriptor s_symdbDescriptor{
    kDescriptorMagic, 0, 0};
#else
__attribute__((section("symdbh"), used)) constinit SymdbDescriptor s_symdbDescriptor{
    kDescriptorMagic, 0, 0};
#endif

struct State {
    std::vector<uint8_t> data;
    const Entry* entries = nullptr;
    uint32_t entryCount = 0;
    const char* strings = nullptr;
    uint64_t stringsLen = 0;
    uintptr_t imageBase = 0;
    // (rva, nameOff) of entries flagged kFlagInlineSites, sorted by rva
    std::vector<std::pair<uint64_t, uint32_t>> inlineSites;
    bool loaded = false;
    bool initialized = false;
};
State s_state;

uint64_t fnv1a64(const char* str) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char* p = str; *p != '\0'; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// Build id of the running executable image, matching what symgen recorded:
// PDB GUID (RFC 4122 byte order) + age on Windows, LC_UUID on Mach-O, GNU
// build-id on ELF. Also reports the address RVAs are relative to.
bool running_image_identity(std::vector<uint8_t>& outId, uintptr_t& outBase) {
#if defined(_WIN32)
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    outBase = reinterpret_cast<uintptr_t>(base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress == 0) {
        return false;
    }
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    for (size_t i = 0; i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) {
            continue;
        }
        struct CvInfo {
            uint32_t signature;  // 'RSDS'
            uint8_t guid[16];
            uint32_t age;
        };
        if (entries[i].SizeOfData < sizeof(CvInfo)) {
            continue;
        }
        const auto* cv = reinterpret_cast<const CvInfo*>(base + entries[i].AddressOfRawData);
        if (cv->signature != 0x53445352) {  // "RSDS"
            continue;
        }
        // The GUID struct stores Data1..Data3 little-endian in memory; the manifest
        // stores RFC 4122 (big-endian) order, so swap them here.
        outId.assign(cv->guid, cv->guid + 16);
        std::swap(outId[0], outId[3]);
        std::swap(outId[1], outId[2]);
        std::swap(outId[4], outId[5]);
        std::swap(outId[6], outId[7]);
        for (int b = 0; b < 4; ++b) {
            outId.push_back(static_cast<uint8_t>(cv->age >> (8 * b)));
        }
        return true;
    }
    return false;
#elif defined(__APPLE__)
    // Image 0 is the main executable. The manifest stores link-time vmaddrs
    // (nm convention, __TEXT vmaddr included).
    const auto* header = _dyld_get_image_header(0);
    outBase = static_cast<uintptr_t>(_dyld_get_image_vmaddr_slide(0));
    const auto* header64 = reinterpret_cast<const mach_header_64*>(header);
    const auto* cmd = reinterpret_cast<const load_command*>(header64 + 1);
    for (uint32_t i = 0; i < header64->ncmds; ++i) {
        if (cmd->cmd == LC_UUID) {
            const auto* uuidCmd = reinterpret_cast<const uuid_command*>(cmd);
            outId.assign(uuidCmd->uuid, uuidCmd->uuid + 16);
            return true;
        }
        cmd = reinterpret_cast<const load_command*>(
            reinterpret_cast<const uint8_t*>(cmd) + cmd->cmdsize);
    }
    return false;
#elif defined(__linux__)
    struct Ctx {
        std::vector<uint8_t>* id;
        uintptr_t probe;
        uintptr_t base = 0;
        bool found = false;
    } ctx{&outId, reinterpret_cast<uintptr_t>(&s_symdbDescriptor)};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            auto* ctx = static_cast<Ctx*>(data);
            // Select the image containing our descriptor: the game is not always the
            // first entry (on Android it is libmain.so, behind the app process).
            bool contains = false;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& phdr = info->dlpi_phdr[i];
                if (phdr.p_type == PT_LOAD && ctx->probe >= info->dlpi_addr + phdr.p_vaddr &&
                    ctx->probe - (info->dlpi_addr + phdr.p_vaddr) < phdr.p_memsz)
                {
                    contains = true;
                    break;
                }
            }
            if (!contains) {
                return 0;
            }
            ctx->base = info->dlpi_addr;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& phdr = info->dlpi_phdr[i];
                if (phdr.p_type != PT_NOTE) {
                    continue;
                }
                const auto* p = reinterpret_cast<const uint8_t*>(info->dlpi_addr + phdr.p_vaddr);
                const auto* end = p + phdr.p_memsz;
                while (p + sizeof(ElfW(Nhdr)) <= end) {
                    const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(p);
                    const auto* name = p + sizeof(ElfW(Nhdr));
                    const auto* desc = name + ((note->n_namesz + 3) & ~3u);
                    if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
                        std::memcmp(name, "GNU", 4) == 0)
                    {
                        ctx->id->assign(desc, desc + note->n_descsz);
                        ctx->found = true;
                        return 1;
                    }
                    p = desc + ((note->n_descsz + 3) & ~3u);
                }
            }
            return 1;  // matched our image; stop either way
        },
        &ctx);
    outBase = ctx.base;
    return ctx.found;
#elif defined(__SWITCH__)
    // aleks2 (H37): the NRO header elf2nro wrote over the start of the image (_start + 0x10,
    // "NRO0") carries the ELF's GNU build id at +0x30, zero-padded to 32 bytes. switch.specs
    // links with --build-id=sha1, so it is the first 20 bytes -- what symgen recorded.
    const auto* image = reinterpret_cast<const uint8_t*>(_start);
    if (std::memcmp(image + 0x10, "NRO0", 4) != 0) {
        return false;
    }
    const uint8_t* id = image + 0x40;
    constexpr size_t kSha1 = 20;
    const bool padded = std::all_of(id + kSha1, id + 32, [](uint8_t b) { return b == 0; });
    outId.assign(id, id + (padded ? kSha1 : 32));
    outBase = reinterpret_cast<uintptr_t>(_start);
    return true;
#else
    (void)outId;
    (void)outBase;
    return false;
#endif
}

std::string hex_string(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        constexpr char kHex[] = "0123456789abcdef";
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

#if defined(__SWITCH__) && DUSK_CODE_MODS
// aleks2 (H36): the Switch build embeds no manifest (symgen and the build id it keys on
// are not part of this build), but every mod is linked into the same executable, so the
// names the built-in mods ask for were resolved by the linker into a table instead
// (switch_builtin_mods.hpp). Said once at startup, with every name the link did not have,
// because a hook that cannot resolve fails its mod's initialisation.
void log_builtin_symbols() {
    size_t total = 0;
    size_t missing = 0;
    for (const auto* entry = loader::detail::kBuiltinSymbols; entry->name != nullptr; entry++) {
        total++;
        if (entry->address == nullptr) {
            missing++;
            Log.warn("link-time symbol table: '{}' is not in this build", entry->name);
        }
    }
    Log.info("no symbol manifest on Switch; link-time symbol table: {} name(s), {} missing", total,
        missing);
}

ResolveStatus resolve_builtin(const char* name, void** outAddr, HookSymbolFlags* outFlags) {
    const auto* entry = loader::builtin_symbol(name != nullptr ? name : "");
    if (entry == nullptr) {
        return ResolveStatus::Unavailable;
    }
    if (entry->address == nullptr) {
        return ResolveStatus::NotFound;
    }
    *outAddr = const_cast<void*>(entry->address);
    if (outFlags != nullptr) {
        *outFlags = entry->code ? HOOK_SYMBOL_CODE : HOOK_SYMBOL_DATA;
    }
    return ResolveStatus::Ok;
}
#endif

void adopt(const uint8_t* blob, size_t blobLen, const std::vector<uint8_t>& imageId, uintptr_t imageBase);

}  // namespace

void initialize() {
    if (s_state.initialized) {
        return;
    }
    s_state.initialized = true;

    if (s_symdbDescriptor.magic != kDescriptorMagic) {
        Log.error("symbol manifest descriptor is corrupt");
        return;
    }
    if (s_symdbDescriptor.rva == 0) {
#if defined(__SWITCH__) && DUSK_CODE_MODS
        log_builtin_symbols();
#endif
#if defined(__SWITCH__)
        Log.info("symbol manifest: romfs:/dusklight.symdb, read when a native mod first needs it");
#else
        Log.info("no symbol manifest embedded; by-name resolution unavailable");
#endif
        return;
    }

    std::vector<uint8_t> imageId;
    uintptr_t imageBase = 0;
    if (!running_image_identity(imageId, imageBase)) {
        Log.error("cannot determine the running image's build id; ignoring symbol manifest");
        return;
    }

    adopt(reinterpret_cast<const uint8_t*>(imageBase + s_symdbDescriptor.rva),
        static_cast<size_t>(s_symdbDescriptor.size), imageId, imageBase);
}

namespace {

// Validates a manifest blob against the running image and, if it is this image's, makes it the
// one resolve() answers from. The embedded manifest (desktop) and the RomFS one (Switch) both
// come through here.
void adopt(const uint8_t* blob, size_t blobLen, const std::vector<uint8_t>& imageId, uintptr_t imageBase) {
    if (blobLen < sizeof(Header)) {
        Log.error("symbol manifest is truncated ({} bytes)", blobLen);
        return;
    }

    Header header{};
    std::memcpy(&header, blob, sizeof(header));
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion) {
        Log.error("symbol manifest has wrong magic/version");
        return;
    }
    const auto compression = static_cast<Compression>(header.compression);
    if ((compression != Compression::None && compression != Compression::Zstd) ||
        header.buildIdLen > sizeof(header.buildId) ||
        header.compressedLen > blobLen - sizeof(Header) ||
        header.uncompressedLen > std::numeric_limits<size_t>::max() ||
        (compression == Compression::None && header.compressedLen != header.uncompressedLen))
    {
        Log.error("symbol manifest is malformed");
        return;
    }

    // The manifest travels inside the image it describes, so a mismatch here means broken
    // build tooling rather than a stale file — but it still guards resolved addresses.
    if (imageId.size() != header.buildIdLen ||
        std::memcmp(imageId.data(), header.buildId, imageId.size()) != 0)
    {
        Log.error("symbol manifest is stale: built for {}, running image is {}",
            hex_string(header.buildId, header.buildIdLen),
            hex_string(imageId.data(), imageId.size()));
        return;
    }

    const auto compressedLen = static_cast<size_t>(header.compressedLen);
    const auto uncompressedLen = static_cast<size_t>(header.uncompressedLen);
    std::vector<uint8_t> data;
    const auto* storedPayload = blob + sizeof(Header);
    if (compression == Compression::None) {
        data.assign(storedPayload, storedPayload + compressedLen);
    } else {
        data.resize(uncompressedLen);
        const size_t decompressedLen =
            ZSTD_decompress(data.data(), data.size(), storedPayload, compressedLen);
        if (ZSTD_isError(decompressedLen)) {
            Log.error("failed to decompress the symbol manifest: {}",
                ZSTD_getErrorName(decompressedLen));
            return;
        }
        if (decompressedLen != data.size()) {
            Log.error("symbol manifest decompressed to {} bytes, expected {}",
                decompressedLen, data.size());
            return;
        }
    }

    const uint64_t entriesEnd = uint64_t{header.entryCount} * sizeof(Entry);
    if (entriesEnd > data.size()) {
        Log.error("decompressed symbol manifest is malformed");
        return;
    }

    s_state.data = std::move(data);
    s_state.entries = reinterpret_cast<const Entry*>(s_state.data.data());
    s_state.entryCount = header.entryCount;
    s_state.strings = reinterpret_cast<const char*>(s_state.data.data() + entriesEnd);
    s_state.stringsLen = s_state.data.size() - entriesEnd;
    s_state.imageBase = imageBase;
    for (uint32_t i = 0; i < s_state.entryCount; ++i) {
        const Entry& entry = s_state.entries[i];
        if ((entry.flags & kFlagInlineSites) != 0 && entry.nameOff < s_state.stringsLen) {
            s_state.inlineSites.emplace_back(entry.rva, entry.nameOff);
        }
    }
    std::sort(s_state.inlineSites.begin(), s_state.inlineSites.end());
    s_state.inlineSites.erase(std::unique(s_state.inlineSites.begin(), s_state.inlineSites.end(),
                                  [](const auto& a, const auto& b) { return a.first == b.first; }),
        s_state.inlineSites.end());
    s_state.loaded = true;
    Log.info("symbol manifest loaded: {} symbols, build id {}", s_state.entryCount,
        hex_string(header.buildId, header.buildIdLen));
}

#if defined(__SWITCH__)
// aleks2 (H37): romfs:/dusklight.symdb, made after the link by SymbolManifestSwitch.cmake.
// Read on first need -- a name the link-time table of the built-in mods does not have, which
// only a native mod loaded at run time asks for -- so a session without one never pays for it.
void load_switch_manifest() {
    static bool s_tried = false;
    if (s_tried) {
        return;
    }
    s_tried = true;
    std::vector<uint8_t> imageId;
    uintptr_t imageBase = 0;
    if (!running_image_identity(imageId, imageBase)) {
        Log.error("no NRO header to take the build id from; by-name resolution unavailable");
        return;
    }
    const auto bootSymT = dusk::boot_timing::now(); std::FILE* file = std::fopen("romfs:/dusklight.symdb", "rb");
    if (file == nullptr) {
        Log.warn("no romfs:/dusklight.symdb in this NRO (built with ALEKS2_SYMBOL_MANIFEST=OFF?); "
                 "native mods cannot hook by name");
        return;
    }
    std::vector<uint8_t> blob;
    uint8_t chunk[64 * 1024];
    for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, file)) > 0;) {
        blob.insert(blob.end(), chunk, chunk + n);
    }
    std::fclose(file);
    adopt(blob.data(), blob.size(), imageId, imageBase); dusk::boot_timing::symbol_manifest(bootSymT);
}
#endif

}  // namespace

bool available() {
    return s_state.loaded;
}

const std::vector<uint8_t>& image_build_id() {
    static const std::vector<uint8_t> s_id = [] {
        std::vector<uint8_t> id;
        uintptr_t base = 0;
        running_image_identity(id, base);
        return id;
    }();
    return s_id;
}

ResolveStatus resolve(const char* name, void** outAddr, HookSymbolFlags* outFlags) {
#if defined(__SWITCH__) && DUSK_CODE_MODS
    // The built-in mods' names were resolved by the linker; anything else is for the manifest.
    if (const auto builtin = resolve_builtin(name, outAddr, outFlags);
        builtin != ResolveStatus::Unavailable)
    {
        return builtin;
    }
#endif
#if defined(__SWITCH__)
    load_switch_manifest();
#endif
    if (!s_state.loaded) {
        return ResolveStatus::Unavailable;
    }
    const uint64_t hash = fnv1a64(name);
    const Entry* begin = s_state.entries;
    const Entry* end = begin + s_state.entryCount;
    size_t lo = 0;
    size_t hi = s_state.entryCount;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (begin[mid].hash < hash) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (const Entry* entry = begin + lo; entry != end && entry->hash == hash; ++entry) {
        if (entry->nameOff >= s_state.stringsLen ||
            std::strcmp(s_state.strings + entry->nameOff, name) != 0)
        {
            continue;
        }
        if ((entry->flags & kFlagDupName) != 0) {
            return ResolveStatus::Ambiguous;
        }
        *outAddr = reinterpret_cast<void*>(s_state.imageBase + entry->rva);
        if (outFlags != nullptr) {
            *outFlags = entry->flags;
        }
        return ResolveStatus::Ok;
    }
    // aleks2 (1.0.2): a name that differs only in its [abi:...] tags (abi_tags.hpp).
    const int64_t index = abi_tags::find_ignoring_tags(name, s_state.entryCount,
        [](uint32_t i) {
            const Entry& entry = s_state.entries[i];
            return entry.nameOff < s_state.stringsLen ? s_state.strings + entry.nameOff : nullptr;
        },
        [](uint32_t i) { return s_state.entries[i].rva; });
    if (index == abi_tags::kNone) {
        return ResolveStatus::NotFound;
    }
    if (index == abi_tags::kAmbiguous || (s_state.entries[index].flags & kFlagDupName) != 0) {
        return ResolveStatus::Ambiguous;
    }
    *outAddr = reinterpret_cast<void*>(s_state.imageBase + s_state.entries[index].rva);
    if (outFlags != nullptr) {
        *outFlags = s_state.entries[index].flags;
    }
    return ResolveStatus::Ok;
}

bool has_inline_sites(const void* addr, const char** outName) {
    if (!s_state.loaded || s_state.inlineSites.empty()) {
        return false;
    }
    const auto rva = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(addr) - s_state.imageBase);
    const auto it = std::lower_bound(s_state.inlineSites.begin(), s_state.inlineSites.end(),
        std::pair<uint64_t, uint32_t>{rva, 0});
    if (it == s_state.inlineSites.end() || it->first != rva) {
        return false;
    }
    if (outName != nullptr) {
        *outName = s_state.strings + it->second;
    }
    return true;
}

// V1-rc16: the name of the code symbol at exactly `addr`, for reports (the hook statistics print game targets hooked
// by address as names instead of raw pointers): a display name when the address has one. A linear scan with no table
// kept -- a few lookups every 30 s cost far less than a sorted copy of the manifest held in memory.
const char* name_at(const void* addr) {
    if (!s_state.loaded) {
        return nullptr;
    }
    const auto rva = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(addr) - s_state.imageBase);
    const char* found = nullptr;
    for (uint32_t i = 0; i < s_state.entryCount; ++i) {
        const Entry& e = s_state.entries[i];
        if (e.rva != rva || (e.flags & kFlagCode) == 0 || e.nameOff >= s_state.stringsLen) {
            continue;
        }
        found = s_state.strings + e.nameOff;
        if ((e.flags & kFlagDisplay) != 0) {
            break;
        }
    }
    return found;
}

}  // namespace dusk::mods::manifest
