#pragma once
// aleks2 (H37): a loader for AArch64 ELF shared objects, for consoles with no dynamic loader.
//
// Horizon has no dlopen. A native mod is an ELF64 AArch64 shared object (lib/<platform>/
// mod.so in its package); this maps one into memory, relocates it and hands back its
// exports. It is written for what mods actually are, measured by
// platforms/switch/tools/elf_audit.py (docs/history/mod-platform.md), not for ELF in
// general: anything it does not handle is refused with a specific reason, never guessed at.
//
// Three stages, each usable on its own:
//
//   parse()  validates the file and builds a model of it: header, segments, dynamic section,
//            dynamic symbols (with versions), relocations, init/fini arrays, unwind tables.
//            Every offset, size and count is checked against the file and against Limits
//            before it is used. Nothing is mapped or executed.
//   plan()   decides, for every relocation, what it will write: a symbol defined by the
//            module itself (always preferred: a module never binds to another module's copy
//            of a definition), an import resolved by the caller (the game's allowlisted
//            surface, or a shim), or null for an unresolved weak import. It collects every
//            problem rather than stopping at the first. This is Dry Run: it allocates no
//            executable memory and runs no code of the module.
//   map()    reserves memory through a MemoryBackend, copies the segments, zeroes .bss,
//            applies the planned relocations, and has the backend publish the image with its
//            final permissions (RX, R, RW; RELRO read-only after relocation; never W+X).
//            Constructors are NOT run by map(): run_constructors() is a separate step the
//            caller takes after every check has passed.
//
// The module is pure C++ with no platform or game dependency, so it is tested on a PC
// (platforms/switch/tools/test-elf-loader.sh), including under qemu-aarch64 with a Linux
// memory backend that really executes the mapped code. The Switch backend is in
// src/dusk/switch_elf_memory.cpp.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::mods::elf {

// Why a module was refused. The names (error_code) are stable: they appear in Dry Run reports.
enum class Error : uint16_t {
    None = 0,
    Truncated,             // the file ends before something the header says is there
    BadMagic,              // not an ELF file
    NotElf64,              // ELFCLASS32
    NotLittleEndian,       // ELFDATA2MSB
    BadVersion,            // e_ident[EI_VERSION] / e_version
    WrongMachine,          // not EM_AARCH64
    WrongType,             // not ET_DYN
    BadHeader,             // e_ehsize/e_phentsize inconsistent
    BadProgramHeaders,     // table out of the file, or overflowing
    TooManySegments,       // over Limits::maxSegments
    NoLoadSegments,        // nothing to map
    SegmentOutOfFile,      // p_offset + p_filesz past the end of the file
    SegmentOverflow,       // an address or size wraps around
    FileSizeExceedsMemSize,
    BadAlignment,          // p_align not a power of two, or offset/vaddr not congruent
    SegmentOverlap,        // two PT_LOAD segments share an address (or a page, with different permissions)
    ImageTooLarge,         // over Limits::maxImageBytes
    WriteExecSegment,      // a segment both writable and executable
    InterpreterPresent,    // PT_INTERP: an executable, not a library
    TlsUnsupported,        // PT_TLS, STT_TLS or a TLS relocation, and the caller offers no TLS (or not this model)
    NoDynamic,             // no PT_DYNAMIC
    BadDynamic,            // malformed dynamic section or missing tables
    AddressNotMapped,      // a dynamic-table address outside every PT_LOAD
    TextRelocations,       // DT_TEXTREL, or a relocation into a non-writable segment
    BadSymbolTable,
    BadStringTable,
    BadVersionTable,
    TooManySymbols,
    BadRelocationTable,
    TooManyRelocations,
    UnsupportedRelocation, // a relocation type the loader does not apply (named in the message)
    RelocationOutOfImage,
    IfuncUnsupported,      // STT_GNU_IFUNC / R_AARCH64_IRELATIVE
    RelrUnsupported,       // DT_RELR
    NeededRejected,        // a DT_NEEDED the loader does not provide
    MissingSymbol,         // an import nothing could resolve
    DeniedSymbol,          // an import the caller refuses (ABI-incompatible runtime symbol)
    MissingExport,         // mod_meta, mod_ctx, mod_initialize, mod_update or mod_shutdown
    BadInitArray,
    MapFailed,             // the memory backend could not reserve
    ProtectFailed,         // the memory backend could not publish
};

const char* error_code(Error error);

struct Limits {
    uint64_t maxFileBytes = 128ull << 20;
    uint64_t maxImageBytes = 64ull << 20;
    uint32_t maxSegments = 16;
    uint64_t maxSymbols = 1u << 20;
    uint64_t maxRelocations = 1u << 22;
    uint64_t maxNeeded = 64;
};

// Segment permission bits, as PF_X/PF_W/PF_R.
inline constexpr uint32_t kPermX = 1;
inline constexpr uint32_t kPermW = 2;
inline constexpr uint32_t kPermR = 4;
inline constexpr uint64_t kPageSize = 0x1000;

struct Segment {
    uint64_t vaddr = 0;
    uint64_t memsz = 0;
    uint64_t offset = 0;
    uint64_t filesz = 0;
    uint64_t align = 0;
    uint32_t perm = 0;
};

struct Symbol {
    std::string_view name;
    std::string_view version;  // from DT_VERNEED/DT_VERDEF, empty if none
    uint64_t value = 0;
    uint64_t size = 0;
    uint8_t bind = 0;  // STB_*
    uint8_t type = 0;  // STT_*
    uint16_t shndx = 0;
    bool defined() const { return shndx != 0; }
};

struct Relocation {
    uint64_t offset = 0;  // vaddr the relocation writes to
    uint32_t type = 0;    // R_AARCH64_*
    uint32_t symbol = 0;  // index into Image::symbols, 0 for none
    int64_t addend = 0;
};

// R_AARCH64_* the loader applies; every other type is refused by name.
inline constexpr uint32_t kRelNone = 0;
inline constexpr uint32_t kRelAbs64 = 257;
inline constexpr uint32_t kRelGlobDat = 1025;
inline constexpr uint32_t kRelJumpSlot = 1026;
inline constexpr uint32_t kRelRelative = 1027;
// TLS-lab: the dynamic TLS relocations, applied only when the caller offers TLS (plan(..., tls=true)).
inline constexpr uint32_t kRelTlsDtpMod64 = 1028;  // module id of a tls_index (__tls_get_addr)
inline constexpr uint32_t kRelTlsDtpRel64 = 1029;  // offset of a tls_index
inline constexpr uint32_t kRelTlsTpRel64 = 1030;   // initial-exec: refused, there is no static TLS block
inline constexpr uint32_t kRelTlsDesc = 1031;      // a TLS descriptor: two words, resolver and argument
const char* relocation_name(uint32_t type);

// TLS-lab: the module's PT_TLS, the template every thread's block of it starts from. vaddr is where
// the initialisation image (.tdata, filesz bytes) lies in the image; the rest of memsz (.tbss) is zero.
struct TlsTemplate {
    uint64_t vaddr = 0;
    uint64_t filesz = 0;
    uint64_t memsz = 0;
    uint64_t align = 1;
};

// A validated model of the file. Views point into the bytes given to parse(), which must
// outlive it.
struct Image {
    std::span<const uint8_t> file;
    std::vector<Segment> loads;
    uint64_t base = 0;  // lowest PT_LOAD vaddr, page-aligned down
    uint64_t size = 0;  // span of the PT_LOAD segments from base, page-aligned up
    uint64_t maxAlign = kPageSize;
    std::optional<Segment> relro;
    std::optional<Segment> ehFrameHdr;
    bool gnuStack = false;
    std::vector<std::string_view> needed;
    std::string_view soname;
    std::vector<Symbol> symbols;  // the dynamic symbol table, index 0 is the null symbol
    std::vector<Relocation> relocations;
    uint64_t initFunc = 0;  // DT_INIT, 0 if none
    uint64_t preinitArray = 0, preinitCount = 0;
    uint64_t initArray = 0, initCount = 0;
    uint64_t finiArray = 0, finiCount = 0;
    uint64_t ehFrame = 0;  // vaddr of .eh_frame found through PT_GNU_EH_FRAME, 0 if none
    bool bindNow = false;
    bool gnuHash = false;
    bool sysvHash = false;
    std::optional<TlsTemplate> tls;  // PT_TLS, if any
    // DM2-d: the program header table as the file has it (phnum entries of 56 bytes), for dl_iterate_phdr.
    std::vector<uint8_t> phdrTable;
    uint16_t phnum = 0;
    bool staticTls = false;          // DF_STATIC_TLS: the module was linked for initial-exec TLS
    bool tlsSymbols = false;         // any STT_TLS symbol, defined or imported
};

struct Diagnostic {
    Error error = Error::None;
    std::string message;
};

struct ParseResult {
    std::optional<Image> image;
    Diagnostic diagnostic;  // error None when image is set
};

ParseResult parse(std::span<const uint8_t> file, const Limits& limits = {});

// Where an import was found.
enum class Source : uint8_t {
    Module,    // defined by the module itself
    Runtime,   // the caller resolved it (the game image, its allowlisted surface)
    Shim,      // the caller resolved it to a stand-in with documented semantics
    WeakNull,  // a weak import nothing defines: null, as the ELF rules say
    Missing,
    Denied,
};
const char* source_name(Source source);

struct Resolved {
    uint64_t address = 0;
    Source source = Source::Runtime;
};

struct ImportQuery {
    std::string_view name;
    std::string_view version;
    bool function = false;
    bool weak = false;
    bool tls = false;  // STT_TLS: the caller must return the address of an accessor, `void* f()`, that
                       // returns the variable's address in the calling thread (TLS-lab)
};

// Resolves one import. Return nullopt when the name is unknown; return Source::Denied to refuse
// it (the reason goes in `why`).
using Resolver = std::function<std::optional<Resolved>(const ImportQuery& query, std::string& why)>;

struct ImportDecision {
    uint32_t symbol = 0;  // index into Image::symbols
    std::string_view name;
    std::string_view version;
    Source source = Source::Missing;
    uint64_t address = 0;  // absolute for Runtime/Shim, module-relative for Module
    std::string why;
    bool tls = false;  // STT_TLS: address is an accessor (imports) or an offset in the module's block
};

// DT_NEEDED decision, made by the caller. Return false (with a reason) to refuse.
using NeededPolicy = std::function<bool(std::string_view soname, std::string& why)>;

struct Plan {
    std::vector<ImportDecision> imports;  // one per distinct undefined symbol referenced
    std::vector<Diagnostic> problems;     // every refusal found, in file order
    std::vector<std::pair<uint32_t, uint64_t>> relocationCounts;  // type -> count
    std::vector<std::pair<std::string_view, uint64_t>> exports;  // mod_* found, module-relative
    bool ok() const { return problems.empty(); }
};

inline constexpr std::string_view kModExports[] = {
    "mod_meta", "mod_ctx", "mod_initialize", "mod_update", "mod_shutdown"};

// tls: whether the caller offers dynamic TLS (TLS-lab). Without it anything thread-local is refused,
// as H38 does; with it TLSDESC, DTPMOD64 and DTPREL64 are planned and TPREL64 (initial-exec, which
// needs a static TLS block allocated with each thread) is still refused.
Plan plan(const Image& image, const Resolver& resolver, const NeededPolicy& needed, bool tls = false);

// Dry Run report, as one JSON object.
std::string report_json(std::string_view label, std::string_view mode, const ParseResult& parsed,
    const Plan* plan, const Diagnostic* mapError = nullptr);

// TLS-lab: where a module's thread-local blocks come from (src/dusk/mods/loader/elf_tls.cpp). map()
// registers the module's template and asks for the words each TLS relocation writes; the blocks
// themselves are allocated per thread, lazily, on the first access from that thread.
class TlsProvider {
public:
    virtual ~TlsProvider() = default;
    // Whatever a module's TLS needs before anything is mapped (the thread-exit key); false, with
    // `error`, refuses the module before any memory is reserved or code run.
    virtual bool prepare(std::string& error) { (void)error; return true; }
    // The module's template; `image` is the runtime address of its initialisation image, readable
    // once the image is published. Returns the module id (>= 1), 0 on failure.
    virtual uint64_t register_module(uint64_t image, const TlsTemplate& tls, std::string& error) = 0;
    // The two words of a TLS descriptor for a variable at `offset` in the module's block.
    virtual std::pair<uint64_t, uint64_t> descriptor(uint64_t module, uint64_t offset) = 0;
    // The two words for an imported variable, reached through `accessor` (ImportQuery::tls).
    virtual std::pair<uint64_t, uint64_t> import_descriptor(uint64_t accessor, int64_t addend) = 0;
    // The module id __tls_get_addr will see for an imported variable (DTPMOD64 against an import):
    // an id that stands for `accessor`, distinct from every registered module's.
    virtual uint64_t import_module(uint64_t accessor) = 0;
    // map() failed after register_module(): the template must never be used (its image is gone).
    virtual void abandon(uint64_t module) { (void)module; }
};

// Memory for one image. reserve() gives a writable staging copy of `size` bytes and the address
// the image will run at (the same pointer on a POSIX host; on Horizon the staging heap is
// remapped as code at runtimeBase). publish() makes it live with the given permissions; the
// staging pointer may be invalid afterwards. Pages never become writable and executable.
struct Protection {
    uint64_t offset = 0;  // from the image base, page-aligned
    uint64_t size = 0;    // page-aligned
    uint32_t perm = 0;    // kPermR | kPermW | kPermX
};

class MemoryBackend {
public:
    virtual ~MemoryBackend() = default;
    virtual bool reserve(uint64_t size, uint64_t align, uint8_t*& staging, uint64_t& runtimeBase,
        std::string& error) = 0;
    virtual bool publish(const std::vector<Protection>& ranges, std::string& error) = 0;
};

// The final permission of every page range of an image (after relocation): each PT_LOAD's own
// flags, RELRO read-only. Exposed for tests and reports.
std::vector<Protection> protections(const Image& image);

// V1-rc7: Horizon lets user code read the physical counter (cntpct_el0, what libnx's armGetSystemTick reads) but
// not the virtual one: Twilit Essentials 2.8.4, built for Linux, reads cntvct_el0 directly in mod_initialize and
// died there on the console (a Data Abort on that very instruction; Ryujinx allows both). The two count the same
// ticks at the same frequency (cntfrq_el0) here, so every `mrs Xt, cntvct_el0` in a module's executable segments
// becomes `mrs Xt, cntpct_el0` before the code is published. Returns how many words were rewritten.
inline uint32_t redirect_counter_reads(uint8_t* code, size_t size) {
    constexpr uint32_t kMrsCntvct = 0xD53BE040u;  // MRS Xt, CNTVCT_EL0 (op0=3 op1=3 CRn=14 CRm=0 op2=2)
    constexpr uint32_t kMrsCntpct = 0xD53BE020u;  // MRS Xt, CNTPCT_EL0 (op2=1)
    uint32_t rewritten = 0;
    for (size_t at = 0; at + 4 <= size; at += 4) {
        uint32_t word;
        std::memcpy(&word, code + at, sizeof word);
        if ((word & 0xFFFFFFE0u) == kMrsCntvct) {
            word = kMrsCntpct | (word & 0x1Fu);
            std::memcpy(code + at, &word, sizeof word);
            ++rewritten;
        }
    }
    return rewritten;
}

struct Loaded {
    uint64_t runtimeBase = 0;  // address of vaddr `image.base`
    uint32_t counterReadsRedirected = 0;  // V1-rc7: redirect_counter_reads
    uint64_t size = 0;
    std::vector<uint64_t> constructors;  // preinit, DT_INIT, init_array, in order (absolute)
    std::vector<std::pair<std::string, uint64_t>> exports;  // every defined global symbol
    uint64_t ehFrame = 0;  // absolute, 0 if none
    uint64_t tlsModule = 0;  // the TlsProvider's id for this module, 0 if it has no PT_TLS

    // Absolute address of a symbol the module defines, 0 if none.
    uint64_t find(std::string_view name) const;
};

struct MapResult {
    std::optional<Loaded> loaded;
    Diagnostic diagnostic;
};

// Requires plan.ok(). Writes nothing executable before every relocation is applied. `tls` is required
// when the plan was made with tls=true and the module uses TLS.
MapResult map(const Image& image, const Plan& plan, MemoryBackend& memory, TlsProvider* tls = nullptr);

// Runs the constructors map() collected. Only after every check has passed: this is the first
// time code of the module executes.
void run_constructors(const Loaded& loaded);

}  // namespace dusk::mods::elf
