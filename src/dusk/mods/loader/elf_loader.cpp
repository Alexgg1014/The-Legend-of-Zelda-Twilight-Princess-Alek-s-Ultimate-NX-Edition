// aleks2 (H37): see elf_loader.hpp.
#include "elf_loader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace dusk::mods::elf {
namespace {

// ELF constants used here (the values are the ABI's; no system header is needed).
constexpr uint8_t kClass64 = 2;
constexpr uint8_t kData2Lsb = 1;
constexpr uint16_t kTypeDyn = 3;
constexpr uint16_t kMachineAarch64 = 183;
constexpr uint32_t kPtLoad = 1, kPtDynamic = 2, kPtInterp = 3, kPtTls = 7;
constexpr uint32_t kPtGnuEhFrame = 0x6474E550, kPtGnuStack = 0x6474E551, kPtGnuRelro = 0x6474E552;
constexpr int64_t kDtNull = 0, kDtNeeded = 1, kDtPltRelSz = 2, kDtHash = 4, kDtStrTab = 5,
                  kDtSymTab = 6, kDtRela = 7, kDtRelaSz = 8, kDtRelaEnt = 9, kDtStrSz = 10,
                  kDtSymEnt = 11, kDtInit = 12, kDtSoname = 14, kDtRel = 17, kDtPltRel = 20,
                  kDtTextRel = 22, kDtJmpRel = 23, kDtBindNow = 24, kDtInitArray = 25,
                  kDtFiniArray = 26, kDtInitArraySz = 27, kDtFiniArraySz = 28, kDtFlags = 30,
                  kDtPreinitArray = 32, kDtPreinitArraySz = 33, kDtRelr = 36;
constexpr int64_t kDtGnuHash = 0x6FFFFEF5, kDtVerSym = 0x6FFFFFF0, kDtFlags1 = 0x6FFFFFFB,
                  kDtVerDef = 0x6FFFFFFC, kDtVerDefNum = 0x6FFFFFFD, kDtVerNeed = 0x6FFFFFFE,
                  kDtVerNeedNum = 0x6FFFFFFF;
constexpr uint64_t kDfTextRel = 0x4, kDfBindNow = 0x8, kDf1Now = 0x1;
constexpr uint8_t kSttTls = 6, kSttGnuIfunc = 10;
constexpr uint8_t kStbLocal = 0, kStbWeak = 2;
constexpr uint8_t kSttFunc = 2;
constexpr uint16_t kShnAbs = 0xFFF1;
constexpr uint32_t kRelIrelative = 1032;
constexpr uint64_t kMaxDynamicEntries = 4096;
constexpr uint64_t kMaxVersionEntries = 4096;
constexpr uint64_t kMaxReserveAlign = 2ull << 20;
constexpr uint64_t kDfStaticTls = 0x10;
constexpr uint64_t kMaxTlsBytes = 1ull << 20;  // per module and thread; the catalog's largest is 11616 (Dawnlight)
constexpr uint64_t kMaxTlsAlign = kPageSize;

template <class T>
T load(const uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

bool add_overflows(uint64_t a, uint64_t b, uint64_t& out) {
    return __builtin_add_overflow(a, b, &out);
}

bool mul_overflows(uint64_t a, uint64_t b, uint64_t& out) {
    return __builtin_mul_overflow(a, b, &out);
}

uint64_t align_down(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

bool align_up(uint64_t v, uint64_t a, uint64_t& out) {
    if (add_overflows(v, a - 1, out)) {
        return false;
    }
    out &= ~(a - 1);
    return true;
}

bool is_pow2(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%#llx", static_cast<unsigned long long>(v));
    return buf;
}

Diagnostic fail(Error error, std::string message) {
    return Diagnostic{error, std::move(message)};
}

// The file, with every read bounds-checked.
struct Reader {
    std::span<const uint8_t> file;

    bool has(uint64_t offset, uint64_t length) const {
        return offset <= file.size() && length <= file.size() - offset;
    }
    const uint8_t* at(uint64_t offset) const { return file.data() + offset; }
};

// Maps vaddr ranges to file bytes through the PT_LOAD segments.
struct AddressMap {
    const std::vector<Segment>* loads = nullptr;

    // File offset of [vaddr, vaddr+length) if it is entirely file-backed in one segment.
    std::optional<uint64_t> file_offset(uint64_t vaddr, uint64_t length) const {
        for (const auto& s : *loads) {
            if (vaddr < s.vaddr) {
                continue;
            }
            const uint64_t into = vaddr - s.vaddr;
            if (into > s.filesz || length > s.filesz - into) {
                continue;
            }
            return s.offset + into;
        }
        return std::nullopt;
    }

    // The segment that contains [vaddr, vaddr+length) in memory (file-backed or .bss).
    const Segment* segment(uint64_t vaddr, uint64_t length) const {
        for (const auto& s : *loads) {
            if (vaddr < s.vaddr) {
                continue;
            }
            const uint64_t into = vaddr - s.vaddr;
            if (into > s.memsz || length > s.memsz - into) {
                continue;
            }
            return &s;
        }
        return nullptr;
    }
};

struct StringTable {
    const uint8_t* data = nullptr;
    uint64_t size = 0;

    std::optional<std::string_view> get(uint64_t offset) const {
        if (offset >= size) {
            return std::nullopt;
        }
        const auto* begin = reinterpret_cast<const char*>(data + offset);
        const auto* end = static_cast<const char*>(std::memchr(begin, 0, size - offset));
        if (end == nullptr) {
            return std::nullopt;
        }
        return std::string_view{begin, static_cast<size_t>(end - begin)};
    }
};

// Number of dynamic symbols, from DT_HASH (nchain) or DT_GNU_HASH (last chain end).
std::optional<uint64_t> count_symbols(const Reader& r, const AddressMap& m, uint64_t hash,
    uint64_t gnuHash, Diagnostic& diag) {
    if (hash != 0) {
        const auto off = m.file_offset(hash, 8);
        if (!off) {
            diag = fail(Error::BadSymbolTable, "DT_HASH outside the file-backed image");
            return std::nullopt;
        }
        return load<uint32_t>(r.at(*off + 4));
    }
    if (gnuHash == 0) {
        diag = fail(Error::BadSymbolTable, "no DT_HASH or DT_GNU_HASH: the symbol count is unknown");
        return std::nullopt;
    }
    const auto head = m.file_offset(gnuHash, 16);
    if (!head) {
        diag = fail(Error::BadSymbolTable, "DT_GNU_HASH outside the file-backed image");
        return std::nullopt;
    }
    const uint32_t nbuckets = load<uint32_t>(r.at(*head));
    const uint32_t symoffset = load<uint32_t>(r.at(*head + 4));
    const uint32_t bloomSize = load<uint32_t>(r.at(*head + 8));
    uint64_t bucketsAt = 0, bloomBytes = 0, bucketBytes = 0;
    if (mul_overflows(bloomSize, 8, bloomBytes) || add_overflows(gnuHash + 16, bloomBytes, bucketsAt) ||
        mul_overflows(nbuckets, 4, bucketBytes))
    {
        diag = fail(Error::BadSymbolTable, "DT_GNU_HASH sizes overflow");
        return std::nullopt;
    }
    const auto buckets = m.file_offset(bucketsAt, bucketBytes);
    if (!buckets) {
        diag = fail(Error::BadSymbolTable, "DT_GNU_HASH buckets outside the image");
        return std::nullopt;
    }
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; ++i) {
        last = std::max(last, load<uint32_t>(r.at(*buckets + 4ull * i)));
    }
    if (last < symoffset) {
        return static_cast<uint64_t>(symoffset);  // every bucket empty
    }
    const uint64_t chainsAt = bucketsAt + bucketBytes;
    for (uint64_t index = last;; ++index) {
        if (index - symoffset > (1ull << 24)) {
            diag = fail(Error::BadSymbolTable, "DT_GNU_HASH chain does not terminate");
            return std::nullopt;
        }
        const auto word = m.file_offset(chainsAt + 4 * (index - symoffset), 4);
        if (!word) {
            diag = fail(Error::BadSymbolTable, "DT_GNU_HASH chain outside the image");
            return std::nullopt;
        }
        if (load<uint32_t>(r.at(*word)) & 1) {
            return index + 1;
        }
    }
}

}  // namespace

const char* error_code(Error error) {
    switch (error) {
    case Error::None: return "none";
    case Error::Truncated: return "truncated";
    case Error::BadMagic: return "bad_magic";
    case Error::NotElf64: return "not_elf64";
    case Error::NotLittleEndian: return "not_little_endian";
    case Error::BadVersion: return "bad_version";
    case Error::WrongMachine: return "wrong_machine";
    case Error::WrongType: return "wrong_type";
    case Error::BadHeader: return "bad_header";
    case Error::BadProgramHeaders: return "bad_program_headers";
    case Error::TooManySegments: return "too_many_segments";
    case Error::NoLoadSegments: return "no_load_segments";
    case Error::SegmentOutOfFile: return "segment_out_of_file";
    case Error::SegmentOverflow: return "segment_overflow";
    case Error::FileSizeExceedsMemSize: return "filesz_exceeds_memsz";
    case Error::BadAlignment: return "bad_alignment";
    case Error::SegmentOverlap: return "segment_overlap";
    case Error::ImageTooLarge: return "image_too_large";
    case Error::WriteExecSegment: return "write_exec_segment";
    case Error::InterpreterPresent: return "interpreter_present";
    case Error::TlsUnsupported: return "tls_unsupported";
    case Error::NoDynamic: return "no_dynamic";
    case Error::BadDynamic: return "bad_dynamic";
    case Error::AddressNotMapped: return "address_not_mapped";
    case Error::TextRelocations: return "text_relocations";
    case Error::BadSymbolTable: return "bad_symbol_table";
    case Error::BadStringTable: return "bad_string_table";
    case Error::BadVersionTable: return "bad_version_table";
    case Error::TooManySymbols: return "too_many_symbols";
    case Error::BadRelocationTable: return "bad_relocation_table";
    case Error::TooManyRelocations: return "too_many_relocations";
    case Error::UnsupportedRelocation: return "unsupported_relocation";
    case Error::RelocationOutOfImage: return "relocation_out_of_image";
    case Error::IfuncUnsupported: return "ifunc_unsupported";
    case Error::RelrUnsupported: return "relr_unsupported";
    case Error::NeededRejected: return "needed_rejected";
    case Error::MissingSymbol: return "missing_symbol";
    case Error::DeniedSymbol: return "denied_symbol";
    case Error::MissingExport: return "missing_export";
    case Error::BadInitArray: return "bad_init_array";
    case Error::MapFailed: return "map_failed";
    case Error::ProtectFailed: return "protect_failed";
    }
    return "unknown";
}

const char* relocation_name(uint32_t type) {
    switch (type) {
    case 0: return "R_AARCH64_NONE";
    case 257: return "R_AARCH64_ABS64";
    case 258: return "R_AARCH64_ABS32";
    case 259: return "R_AARCH64_ABS16";
    case 260: return "R_AARCH64_PREL64";
    case 261: return "R_AARCH64_PREL32";
    case 1024: return "R_AARCH64_COPY";
    case 1025: return "R_AARCH64_GLOB_DAT";
    case 1026: return "R_AARCH64_JUMP_SLOT";
    case 1027: return "R_AARCH64_RELATIVE";
    case 1028: return "R_AARCH64_TLS_DTPMOD64";
    case 1029: return "R_AARCH64_TLS_DTPREL64";
    case 1030: return "R_AARCH64_TLS_TPREL64";
    case 1031: return "R_AARCH64_TLSDESC";
    case 1032: return "R_AARCH64_IRELATIVE";
    }
    return "R_AARCH64_unknown";
}

const char* source_name(Source source) {
    switch (source) {
    case Source::Module: return "module";
    case Source::Runtime: return "runtime";
    case Source::Shim: return "shim";
    case Source::WeakNull: return "weak_null";
    case Source::Missing: return "missing";
    case Source::Denied: return "denied";
    }
    return "unknown";
}

ParseResult parse(std::span<const uint8_t> file, const Limits& limits) {
    ParseResult result;
    auto& diag = result.diagnostic;
    const Reader r{file};
    if (file.size() > limits.maxFileBytes) {
        diag = fail(Error::ImageTooLarge, "file is " + std::to_string(file.size()) + " bytes, over the limit");
        return result;
    }
    if (!r.has(0, 64)) {
        diag = fail(Error::Truncated, "shorter than an ELF64 header");
        return result;
    }
    const uint8_t* h = r.at(0);
    if (std::memcmp(h, "\x7f" "ELF", 4) != 0) {
        diag = fail(Error::BadMagic, "not an ELF file");
        return result;
    }
    if (h[4] != kClass64) {
        diag = fail(Error::NotElf64, "ELF class " + std::to_string(h[4]) + ", not ELF64");
        return result;
    }
    if (h[5] != kData2Lsb) {
        diag = fail(Error::NotLittleEndian, "big-endian ELF");
        return result;
    }
    if (h[6] != 1 || load<uint32_t>(h + 20) != 1) {
        diag = fail(Error::BadVersion, "ELF version is not 1");
        return result;
    }
    const uint16_t type = load<uint16_t>(h + 16);
    const uint16_t machine = load<uint16_t>(h + 18);
    if (machine != kMachineAarch64) {
        diag = fail(Error::WrongMachine, "machine " + std::to_string(machine) + ", not AArch64 (183)");
        return result;
    }
    if (type != kTypeDyn) {
        diag = fail(Error::WrongType, "e_type " + std::to_string(type) + ", not a shared object (ET_DYN)");
        return result;
    }
    const uint64_t phoff = load<uint64_t>(h + 32);
    const uint16_t ehsize = load<uint16_t>(h + 52);
    const uint16_t phentsize = load<uint16_t>(h + 54);
    const uint16_t phnum = load<uint16_t>(h + 56);
    if (ehsize < 64 || phentsize != 56) {
        diag = fail(Error::BadHeader, "e_ehsize/e_phentsize are not ELF64's");
        return result;
    }
    if (phnum > limits.maxSegments * 4u + 16u) {
        diag = fail(Error::TooManySegments, std::to_string(phnum) + " program headers");
        return result;
    }
    uint64_t tableBytes = 0;
    if (mul_overflows(phnum, 56, tableBytes) || !r.has(phoff, tableBytes)) {
        diag = fail(Error::BadProgramHeaders, "program header table is not inside the file");
        return result;
    }

    Image image;
    image.file = file;
    image.phdrTable.assign(file.begin() + static_cast<std::ptrdiff_t>(phoff),
        file.begin() + static_cast<std::ptrdiff_t>(phoff + tableBytes));
    image.phnum = phnum;
    std::optional<Segment> dynamic;
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t* p = r.at(phoff + 56ull * i);
        const uint32_t ptype = load<uint32_t>(p);
        Segment s;
        s.perm = load<uint32_t>(p + 4) & 7u;
        s.offset = load<uint64_t>(p + 8);
        s.vaddr = load<uint64_t>(p + 16);
        s.filesz = load<uint64_t>(p + 32);
        s.memsz = load<uint64_t>(p + 40);
        s.align = load<uint64_t>(p + 48);
        uint64_t end = 0;
        switch (ptype) {
        case kPtLoad:
            if (s.filesz > s.memsz) {
                diag = fail(Error::FileSizeExceedsMemSize, "PT_LOAD at " + hex(s.vaddr) + " has p_filesz > p_memsz");
                return result;
            }
            if (!r.has(s.offset, s.filesz)) {
                diag = fail(Error::SegmentOutOfFile, "PT_LOAD at " + hex(s.vaddr) + " runs past the end of the file");
                return result;
            }
            if (add_overflows(s.vaddr, s.memsz, end) || !align_up(end, kPageSize, end)) {
                diag = fail(Error::SegmentOverflow, "PT_LOAD at " + hex(s.vaddr) + " wraps the address space");
                return result;
            }
            if (s.align > 1 && (!is_pow2(s.align) || (s.offset % s.align) != (s.vaddr % s.align))) {
                diag = fail(Error::BadAlignment, "PT_LOAD at " + hex(s.vaddr) + " has p_align " + hex(s.align) +
                                                     " that is not a power of two congruent with its offset");
                return result;
            }
            if (s.align > kMaxReserveAlign) {
                diag = fail(Error::BadAlignment, "PT_LOAD alignment " + hex(s.align) + " over 2 MiB");
                return result;
            }
            if ((s.perm & kPermW) && (s.perm & kPermX)) {
                diag = fail(Error::WriteExecSegment, "PT_LOAD at " + hex(s.vaddr) + " is writable and executable");
                return result;
            }
            if (image.loads.size() >= limits.maxSegments) {
                diag = fail(Error::TooManySegments, "more than " + std::to_string(limits.maxSegments) + " PT_LOAD segments");
                return result;
            }
            if (s.memsz != 0) {
                image.loads.push_back(s);
            }
            break;
        case kPtDynamic:
            dynamic = s;
            break;
        case kPtInterp:
            diag = fail(Error::InterpreterPresent, "has PT_INTERP: an executable, not a library");
            return result;
        case kPtTls:
            // TLS-lab: kept as the module's template; plan() refuses it unless the caller offers TLS.
            if (image.tls) {
                diag = fail(Error::BadProgramHeaders, "more than one PT_TLS");
                return result;
            }
            if (s.filesz > s.memsz || s.memsz > kMaxTlsBytes || (s.align > 1 && !is_pow2(s.align)) ||
                s.align > kMaxTlsAlign)
            {
                diag = fail(Error::BadProgramHeaders, "PT_TLS sizes or alignment are not acceptable (memsz " +
                                                          std::to_string(s.memsz) + ", align " + hex(s.align) + ")");
                return result;
            }
            image.tls = TlsTemplate{s.vaddr, s.filesz, s.memsz, std::max<uint64_t>(s.align, 1)};
            break;
        case kPtGnuRelro:
            image.relro = s;
            break;
        case kPtGnuEhFrame:
            image.ehFrameHdr = s;
            break;
        case kPtGnuStack:
            image.gnuStack = true;
            break;
        default:
            break;
        }
    }
    if (image.loads.empty()) {
        diag = fail(Error::NoLoadSegments, "no PT_LOAD segment");
        return result;
    }
    std::ranges::sort(image.loads, {}, &Segment::vaddr);
    for (size_t i = 1; i < image.loads.size(); ++i) {
        const auto& prev = image.loads[i - 1];
        const auto& cur = image.loads[i];
        if (prev.vaddr + prev.memsz > cur.vaddr) {
            diag = fail(Error::SegmentOverlap, "PT_LOAD at " + hex(prev.vaddr) + " overlaps the one at " + hex(cur.vaddr));
            return result;
        }
        uint64_t prevEnd = 0;
        align_up(prev.vaddr + prev.memsz, kPageSize, prevEnd);
        if (prevEnd > align_down(cur.vaddr, kPageSize) && prev.perm != cur.perm) {
            diag = fail(Error::SegmentOverlap, "PT_LOADs at " + hex(prev.vaddr) + " and " + hex(cur.vaddr) +
                                                   " share a page with different permissions");
            return result;
        }
    }
    image.base = align_down(image.loads.front().vaddr, kPageSize);
    uint64_t end = 0;
    for (const auto& s : image.loads) {
        uint64_t segEnd = 0;
        align_up(s.vaddr + s.memsz, kPageSize, segEnd);
        end = std::max(end, segEnd);
        image.maxAlign = std::max(image.maxAlign, s.align);
    }
    image.size = end - image.base;
    if (image.size > limits.maxImageBytes) {
        diag = fail(Error::ImageTooLarge, "image spans " + std::to_string(image.size) + " bytes, over the limit of " +
                                              std::to_string(limits.maxImageBytes));
        return result;
    }

    // PT_GNU_RELRO names pages that become read-only after relocation: it must lie inside the
    // image (found by the fuzz corpus: a wild one made protections() walk 2^52 pages).
    if (image.relro) {
        const auto& g = *image.relro;
        if (g.vaddr < image.base || g.memsz > image.size || g.vaddr - image.base > image.size - g.memsz) {
            diag = fail(Error::BadProgramHeaders, "PT_GNU_RELRO at " + hex(g.vaddr) + " lies outside the image");
            return result;
        }
    }

    const AddressMap m{&image.loads};
    // TLS-lab: the initialisation image is read from the mapped image by every thread's first access,
    // so it must lie in file-backed memory of one PT_LOAD.
    if (image.tls && image.tls->filesz != 0 && !m.file_offset(image.tls->vaddr, image.tls->filesz)) {
        diag = fail(Error::BadProgramHeaders, "PT_TLS initialisation image at " + hex(image.tls->vaddr) +
                                                  " is not inside one PT_LOAD's file-backed bytes");
        return result;
    }
    if (!dynamic) {
        diag = fail(Error::NoDynamic, "no PT_DYNAMIC: nothing says how to relocate it");
        return result;
    }
    if (!r.has(dynamic->offset, dynamic->filesz) || dynamic->filesz % 16 != 0) {
        diag = fail(Error::BadDynamic, "PT_DYNAMIC is not inside the file");
        return result;
    }
    if (dynamic->filesz / 16 > kMaxDynamicEntries) {
        diag = fail(Error::BadDynamic, "PT_DYNAMIC has too many entries");
        return result;
    }

    std::map<int64_t, uint64_t> dyn;
    std::vector<uint64_t> neededOffsets;
    for (uint64_t at = dynamic->offset; at + 16 <= dynamic->offset + dynamic->filesz; at += 16) {
        const int64_t tag = load<int64_t>(r.at(at));
        const uint64_t value = load<uint64_t>(r.at(at + 8));
        if (tag == kDtNull) {
            break;
        }
        if (tag == kDtNeeded) {
            if (neededOffsets.size() >= limits.maxNeeded) {
                diag = fail(Error::BadDynamic, "too many DT_NEEDED entries");
                return result;
            }
            neededOffsets.push_back(value);
        } else {
            dyn[tag] = value;
        }
    }
    const auto get = [&](int64_t tag) -> uint64_t {
        const auto it = dyn.find(tag);
        return it == dyn.end() ? 0 : it->second;
    };
    if (dyn.contains(kDtTextRel) || (get(kDtFlags) & kDfTextRel)) {
        diag = fail(Error::TextRelocations, "has text relocations (DT_TEXTREL)");
        return result;
    }
    if (dyn.contains(kDtRelr)) {
        diag = fail(Error::RelrUnsupported, "uses packed relative relocations (DT_RELR)");
        return result;
    }
    if (dyn.contains(kDtRel)) {
        diag = fail(Error::BadDynamic, "uses DT_REL relocations; AArch64 uses DT_RELA");
        return result;
    }
    if (dyn.contains(kDtPltRel) && get(kDtPltRel) != kDtRela) {
        diag = fail(Error::BadDynamic, "DT_PLTREL is not DT_RELA");
        return result;
    }
    if ((dyn.contains(kDtSymEnt) && get(kDtSymEnt) != 24) || (dyn.contains(kDtRelaEnt) && get(kDtRelaEnt) != 24)) {
        diag = fail(Error::BadDynamic, "DT_SYMENT/DT_RELAENT are not ELF64's");
        return result;
    }
    image.bindNow = dyn.contains(kDtBindNow) || (get(kDtFlags) & kDfBindNow) || (get(kDtFlags1) & kDf1Now);
    image.staticTls = (get(kDtFlags) & kDfStaticTls) != 0;
    image.gnuHash = dyn.contains(kDtGnuHash);
    image.sysvHash = dyn.contains(kDtHash);

    // String table.
    if (!dyn.contains(kDtStrTab) || !dyn.contains(kDtSymTab)) {
        diag = fail(Error::BadDynamic, "no DT_STRTAB or DT_SYMTAB");
        return result;
    }
    const auto strOff = m.file_offset(get(kDtStrTab), get(kDtStrSz));
    if (!strOff || get(kDtStrSz) == 0) {
        diag = fail(Error::BadStringTable, "DT_STRTAB/DT_STRSZ is not inside the file-backed image");
        return result;
    }
    const StringTable strings{r.at(*strOff), get(kDtStrSz)};
    for (const uint64_t off : neededOffsets) {
        const auto name = strings.get(off);
        if (!name) {
            diag = fail(Error::BadStringTable, "DT_NEEDED name outside DT_STRTAB");
            return result;
        }
        image.needed.push_back(*name);
    }
    if (dyn.contains(kDtSoname)) {
        const auto name = strings.get(get(kDtSoname));
        if (!name) {
            diag = fail(Error::BadStringTable, "DT_SONAME outside DT_STRTAB");
            return result;
        }
        image.soname = *name;
    }

    // Symbols.
    const auto count = count_symbols(r, m, get(kDtHash), get(kDtGnuHash), diag);
    if (!count) {
        return result;
    }
    if (*count > limits.maxSymbols) {
        diag = fail(Error::TooManySymbols, std::to_string(*count) + " dynamic symbols");
        return result;
    }
    uint64_t symBytes = 0;
    mul_overflows(*count, 24, symBytes);
    const auto symOff = m.file_offset(get(kDtSymTab), symBytes);
    if (!symOff) {
        diag = fail(Error::BadSymbolTable, "DT_SYMTAB is not inside the file-backed image");
        return result;
    }
    image.symbols.reserve(*count);
    for (uint64_t i = 0; i < *count; ++i) {
        const uint8_t* s = r.at(*symOff + 24 * i);
        Symbol sym;
        const auto name = strings.get(load<uint32_t>(s));
        if (!name) {
            diag = fail(Error::BadStringTable, "symbol " + std::to_string(i) + " has a name outside DT_STRTAB");
            return result;
        }
        sym.name = *name;
        sym.bind = s[4] >> 4;
        sym.type = s[4] & 0xF;
        sym.shndx = load<uint16_t>(s + 6);
        sym.value = load<uint64_t>(s + 8);
        sym.size = load<uint64_t>(s + 16);
        if (sym.type == kSttTls) {
            // TLS-lab: a defined one's value is an offset in the module's TLS block, not an address.
            image.tlsSymbols = true;
            if (sym.defined() && (!image.tls || sym.value > image.tls->memsz ||
                                     sym.size > image.tls->memsz - sym.value))
            {
                diag = fail(Error::BadSymbolTable, "thread-local symbol '" + std::string(sym.name) +
                                                       "' lies outside the module's PT_TLS");
                return result;
            }
        }
        if (sym.type == kSttGnuIfunc) {
            diag = fail(Error::IfuncUnsupported, "indirect function '" + std::string(sym.name) + "'");
            return result;
        }
        if (sym.defined() && sym.type != kSttTls && sym.shndx != kShnAbs && sym.bind != kStbLocal &&
            m.segment(sym.value, 0) == nullptr)
        {
            diag = fail(Error::BadSymbolTable, "symbol '" + std::string(sym.name) + "' defined outside the image");
            return result;
        }
        image.symbols.push_back(sym);
    }

    // Symbol versions (for reports and for callers that refuse a runtime's versioned names).
    if (dyn.contains(kDtVerSym)) {
        const auto versymOff = m.file_offset(get(kDtVerSym), symBytes / 12);
        if (!versymOff) {
            diag = fail(Error::BadVersionTable, "DT_VERSYM is not inside the file-backed image");
            return result;
        }
        std::map<uint16_t, std::string_view> names;
        const auto walk = [&](uint64_t start, uint64_t num, bool needs) -> bool {
            uint64_t at = start;
            for (uint64_t n = 0; n < num && n < kMaxVersionEntries; ++n) {
                // Elf64_Verneed is 16 bytes, Elf64_Verdef 20; their counts sit at 2 and 6.
                const auto entry = m.file_offset(at, needs ? 16 : 20);
                if (!entry) {
                    return false;
                }
                const uint8_t* e = r.at(*entry);
                const uint16_t cnt = load<uint16_t>(e + (needs ? 2 : 6));
                const uint32_t auxOff = load<uint32_t>(e + (needs ? 8 : 12));
                const uint32_t next = load<uint32_t>(e + (needs ? 12 : 16));
                uint64_t aux = at + auxOff;
                for (uint16_t k = 0; k < cnt && k < 64; ++k) {
                    const auto a = m.file_offset(aux, needs ? 16 : 8);
                    if (!a) {
                        return false;
                    }
                    const uint8_t* ae = r.at(*a);
                    const uint32_t nameOff = load<uint32_t>(ae + (needs ? 8 : 0));
                    const auto name = strings.get(nameOff);
                    if (!name) {
                        return false;
                    }
                    if (needs) {
                        names[load<uint16_t>(ae + 6) & 0x7FFF] = *name;
                    } else if (k == 0) {
                        names[load<uint16_t>(e + 4) & 0x7FFF] = *name;
                    }
                    const uint32_t auxNext = load<uint32_t>(ae + (needs ? 12 : 4));
                    if (auxNext == 0) {
                        break;
                    }
                    aux += auxNext;
                }
                if (next == 0) {
                    break;
                }
                at += next;
            }
            return true;
        };
        if ((dyn.contains(kDtVerNeed) && !walk(get(kDtVerNeed), get(kDtVerNeedNum), true)) ||
            (dyn.contains(kDtVerDef) && !walk(get(kDtVerDef), get(kDtVerDefNum), false)))
        {
            diag = fail(Error::BadVersionTable, "symbol version tables are malformed");
            return result;
        }
        for (uint64_t i = 0; i < *count; ++i) {
            const uint16_t index = load<uint16_t>(r.at(*versymOff + 2 * i)) & 0x7FFF;
            if (index >= 2) {
                if (const auto it = names.find(index); it != names.end()) {
                    image.symbols[i].version = it->second;
                }
            }
        }
    }

    // Relocations: DT_RELA and the PLT's DT_JMPREL.
    const auto readRelocs = [&](int64_t tableTag, int64_t sizeTag, const char* what) -> bool {
        if (!dyn.contains(tableTag)) {
            return true;
        }
        const uint64_t bytes = get(sizeTag);
        if (bytes % 24 != 0) {
            diag = fail(Error::BadRelocationTable, std::string(what) + " size is not a multiple of 24");
            return false;
        }
        const auto off = m.file_offset(get(tableTag), bytes);
        if (!off) {
            diag = fail(Error::BadRelocationTable, std::string(what) + " is not inside the file-backed image");
            return false;
        }
        if (image.relocations.size() + bytes / 24 > limits.maxRelocations) {
            diag = fail(Error::TooManyRelocations, "more than " + std::to_string(limits.maxRelocations) + " relocations");
            return false;
        }
        for (uint64_t i = 0; i < bytes / 24; ++i) {
            const uint8_t* e = r.at(*off + 24 * i);
            Relocation rel;
            rel.offset = load<uint64_t>(e);
            const uint64_t info = load<uint64_t>(e + 8);
            rel.type = static_cast<uint32_t>(info & 0xFFFFFFFF);
            rel.symbol = static_cast<uint32_t>(info >> 32);
            rel.addend = load<int64_t>(e + 16);
            if (rel.symbol >= image.symbols.size()) {
                diag = fail(Error::BadRelocationTable, std::string(what) + " entry " + std::to_string(i) +
                                                           " names symbol " + std::to_string(rel.symbol) + " of " +
                                                           std::to_string(image.symbols.size()));
                return false;
            }
            if (rel.type == kRelIrelative) {
                diag = fail(Error::IfuncUnsupported, "R_AARCH64_IRELATIVE relocation");
                return false;
            }
            image.relocations.push_back(rel);
        }
        return true;
    };
    if (!readRelocs(kDtRela, kDtRelaSz, "DT_RELA") || !readRelocs(kDtJmpRel, kDtPltRelSz, "DT_JMPREL")) {
        return result;
    }

    // Constructor and destructor arrays: inside the image's memory, whole pointers.
    const auto arrayOf = [&](int64_t addrTag, int64_t sizeTag, uint64_t& addr, uint64_t& countOut) -> bool {
        if (!dyn.contains(addrTag)) {
            return true;
        }
        const uint64_t bytes = get(sizeTag);
        addr = get(addrTag);
        if (bytes % 8 != 0 || bytes / 8 > 65536 || m.segment(addr, bytes) == nullptr) {
            diag = fail(Error::BadInitArray, "init/fini array is malformed or outside the image");
            return false;
        }
        countOut = bytes / 8;
        return true;
    };
    if (!arrayOf(kDtPreinitArray, kDtPreinitArraySz, image.preinitArray, image.preinitCount) ||
        !arrayOf(kDtInitArray, kDtInitArraySz, image.initArray, image.initCount) ||
        !arrayOf(kDtFiniArray, kDtFiniArraySz, image.finiArray, image.finiCount))
    {
        return result;
    }
    if (dyn.contains(kDtInit)) {
        image.initFunc = get(kDtInit);
        const Segment* seg = m.segment(image.initFunc, 4);
        if (seg == nullptr || !(seg->perm & kPermX)) {
            diag = fail(Error::BadInitArray, "DT_INIT is not in an executable segment");
            return result;
        }
    }

    // .eh_frame through PT_GNU_EH_FRAME (eh_frame_hdr: version, eh_frame_ptr encoding, ...).
    if (image.ehFrameHdr) {
        if (const auto hdr = m.file_offset(image.ehFrameHdr->vaddr, 12); hdr && r.at(*hdr)[0] == 1) {
            const uint8_t enc = r.at(*hdr)[1];
            const uint64_t field = image.ehFrameHdr->vaddr + 4;
            if (enc == 0x1B) {  // DW_EH_PE_pcrel | DW_EH_PE_sdata4
                image.ehFrame = field + static_cast<int64_t>(load<int32_t>(r.at(*hdr + 4)));
            } else if (enc == 0x03 || enc == 0x0B) {  // udata4 / sdata4, absolute
                image.ehFrame = load<uint32_t>(r.at(*hdr + 4));
            } else if (enc == 0x1C) {  // pcrel | sdata8
                image.ehFrame = field + load<int64_t>(r.at(*hdr + 4));
            }
            if (image.ehFrame != 0 && m.segment(image.ehFrame, 4) == nullptr) {
                image.ehFrame = 0;
            }
        }
    }

    result.image = std::move(image);
    return result;
}

Plan plan(const Image& image, const Resolver& resolver, const NeededPolicy& needed, bool tls) {
    Plan out;
    const AddressMap m{&image.loads};

    // Thread-local storage first, as parse() used to refuse it: the first problem is what a report and
    // the boot guard's category name.
    if (!tls && (image.tls || image.tlsSymbols)) {
        out.problems.push_back(fail(Error::TlsUnsupported,
            "has thread-local storage (PT_TLS / TLS symbols), which the loader does not provide"));
    }
    if (tls && image.staticTls) {
        out.problems.push_back(fail(Error::TlsUnsupported,
            "linked for initial-exec TLS (DF_STATIC_TLS): it needs a static TLS block allocated with every "
            "thread, and the loader provides only dynamic TLS (TLSDESC, __tls_get_addr)"));
    }

    for (const auto soname : image.needed) {
        std::string why;
        if (!needed || !needed(soname, why)) {
            out.problems.push_back(fail(Error::NeededRejected, "DT_NEEDED '" + std::string(soname) + "': " +
                                                                   (why.empty() ? "not provided" : why)));
        }
    }

    // Relocation types, targets.
    std::map<uint32_t, uint64_t> counts;
    std::map<uint32_t, uint64_t> unsupported;
    uint64_t textRelocs = 0, outside = 0;
    uint64_t firstText = 0, firstOutside = 0;
    std::set<uint32_t> referenced;
    uint64_t tprel = 0, tlsMisuse = 0;
    std::string firstMisuse;
    const auto misuse = [&](std::string what) {
        if (tlsMisuse++ == 0) {
            firstMisuse = std::move(what);
        }
    };
    for (const auto& rel : image.relocations) {
        ++counts[rel.type];
        uint64_t width = 8;
        switch (rel.type) {
        case kRelNone:
            continue;
        case kRelRelative:
            break;
        case kRelAbs64:
        case kRelGlobDat:
        case kRelJumpSlot:
            if (rel.symbol != 0) {
                if (image.symbols[rel.symbol].type == kSttTls) {
                    misuse(std::string(relocation_name(rel.type)) + " against thread-local '" +
                           std::string(image.symbols[rel.symbol].name) + "'");
                    continue;
                }
                referenced.insert(rel.symbol);
            }
            break;
        case kRelTlsDesc:
        case kRelTlsDtpMod64:
        case kRelTlsDtpRel64: {
            if (!tls) {
                ++unsupported[rel.type];
                continue;
            }
            // TLS-lab. A TLSDESC fills two words (the GOT pair); the tls_index pair is two
            // relocations of one word each.
            width = rel.type == kRelTlsDesc ? 16 : 8;
            if (rel.symbol == 0) {
                if (!image.tls && rel.type != kRelTlsDtpMod64) {
                    misuse(std::string(relocation_name(rel.type)) + " for the module's own TLS, but it has no PT_TLS");
                    continue;
                }
            } else {
                const Symbol& sym = image.symbols[rel.symbol];
                if (sym.type != kSttTls) {
                    misuse(std::string(relocation_name(rel.type)) + " against '" + std::string(sym.name) +
                           "', which is not thread-local");
                    continue;
                }
                // An import (the game's thread-local) gets, for __tls_get_addr, a module id of its own
                // that stands for its accessor (TlsProvider::import_module).
                referenced.insert(rel.symbol);
            }
            break;
        }
        case kRelTlsTpRel64:
            if (!tls) {
                ++unsupported[rel.type];
            } else {
                ++tprel;
            }
            continue;
        default:
            ++unsupported[rel.type];
            continue;
        }
        const Segment* seg = m.segment(rel.offset, width);
        if (seg == nullptr) {
            if (outside++ == 0) {
                firstOutside = rel.offset;
            }
        } else if (!(seg->perm & kPermW)) {
            if (textRelocs++ == 0) {
                firstText = rel.offset;
            }
        }
    }
    for (const auto& [type, n] : unsupported) {
        out.problems.push_back(fail(Error::UnsupportedRelocation,
            std::string(relocation_name(type)) + " (" + std::to_string(type) + ") x" + std::to_string(n) +
                (tls ? ": the loader applies only NONE, ABS64, GLOB_DAT, JUMP_SLOT, RELATIVE, TLSDESC, "
                       "TLS_DTPMOD64 and TLS_DTPREL64" :
                       ": the loader applies only NONE, ABS64, GLOB_DAT, JUMP_SLOT and RELATIVE")));
    }
    if (tprel != 0) {
        out.problems.push_back(fail(Error::TlsUnsupported,
            "R_AARCH64_TLS_TPREL64 x" + std::to_string(tprel) +
                ": initial-exec TLS needs a static TLS block allocated with every thread; the loader provides "
                "only dynamic TLS (TLSDESC, __tls_get_addr)"));
    }
    if (tlsMisuse != 0) {
        out.problems.push_back(fail(Error::BadRelocationTable,
            std::to_string(tlsMisuse) + " malformed thread-local relocation(s), first: " + firstMisuse));
    }
    if (outside != 0) {
        out.problems.push_back(fail(Error::RelocationOutOfImage,
            std::to_string(outside) + " relocation(s) outside the image, first at " + hex(firstOutside)));
    }
    if (textRelocs != 0) {
        out.problems.push_back(fail(Error::TextRelocations,
            std::to_string(textRelocs) + " relocation(s) into a read-only segment, first at " + hex(firstText)));
    }
    out.relocationCounts.assign(counts.begin(), counts.end());

    // Imports: the module's own definitions first, always; then the caller.
    std::vector<std::string_view> missing, denied;
    for (const uint32_t index : referenced) {
        const Symbol& sym = image.symbols[index];
        ImportDecision d;
        d.symbol = index;
        d.name = sym.name;
        d.version = sym.version;
        d.tls = sym.type == kSttTls;
        if (sym.defined()) {
            d.source = Source::Module;
            d.address = sym.value;  // for a thread-local, the offset in the module's block
            out.imports.push_back(std::move(d));
            continue;
        }
        const ImportQuery query{sym.name, sym.version, sym.type == kSttFunc, sym.bind == kStbWeak, d.tls};
        std::string why;
        const auto r = resolver ? resolver(query, why) : std::nullopt;
        if (r && r->source == Source::Denied) {
            d.source = Source::Denied;
            d.why = why;
            denied.push_back(sym.name);
            out.problems.push_back(fail(Error::DeniedSymbol, "'" + std::string(sym.name) + "' refused: " + why));
        } else if (r) {
            d.source = r->source;
            d.address = r->address;
            d.why = why;
        } else if (sym.bind == kStbWeak && !d.tls) {  // a thread-local has no null address to stand for it
            d.source = Source::WeakNull;
        } else {
            d.source = Source::Missing;
            d.why = why;
            missing.push_back(sym.name);
        }
        out.imports.push_back(std::move(d));
    }
    if (!missing.empty()) {
        std::string names;
        for (size_t i = 0; i < missing.size() && i < 12; ++i) {
            names += (i ? ", " : "") + std::string(missing[i]);
        }
        if (missing.size() > 12) {
            names += ", ... (" + std::to_string(missing.size()) + " in all)";
        }
        out.problems.push_back(fail(Error::MissingSymbol, "unresolved import(s): " + names));
    }

    // The five exports the mod loader needs.
    std::vector<std::string_view> absent;
    for (const auto name : kModExports) {
        const auto it = std::ranges::find_if(image.symbols, [&](const Symbol& s) {
            return s.name == name && s.defined() && s.bind != kStbLocal;
        });
        if (it == image.symbols.end()) {
            absent.push_back(name);
        } else {
            out.exports.emplace_back(name, it->value);
        }
    }
    if (!absent.empty()) {
        std::string names;
        for (size_t i = 0; i < absent.size(); ++i) {
            names += (i ? ", " : "") + std::string(absent[i]);
        }
        out.problems.push_back(fail(Error::MissingExport, "does not export " + names));
    }
    return out;
}

std::vector<Protection> protections(const Image& image) {
    const uint64_t pages = image.size / kPageSize;
    std::vector<uint32_t> perm(pages, 0);
    for (const auto& s : image.loads) {
        const uint64_t first = (align_down(s.vaddr, kPageSize) - image.base) / kPageSize;
        uint64_t end = 0;
        align_up(s.vaddr + s.memsz, kPageSize, end);
        for (uint64_t p = first; p < (end - image.base) / kPageSize && p < pages; ++p) {
            perm[p] = s.perm;
        }
    }
    if (image.relro && image.relro->memsz != 0) {
        // Whole pages inside PT_GNU_RELRO become read-only once relocated.
        // parse() keeps it inside the image, so the page indices below cannot wrap.
        uint64_t first = 0;
        align_up(image.relro->vaddr - image.base, kPageSize, first);
        const uint64_t last = align_down(image.relro->vaddr - image.base + image.relro->memsz, kPageSize);
        for (uint64_t p = first / kPageSize; p < last / kPageSize && p < pages; ++p) {
            perm[p] &= ~kPermW;
        }
    }
    std::vector<Protection> out;
    for (uint64_t p = 0; p < pages;) {
        uint64_t q = p;
        while (q < pages && perm[q] == perm[p]) {
            ++q;
        }
        out.push_back({p * kPageSize, (q - p) * kPageSize, perm[p]});
        p = q;
    }
    return out;
}

uint64_t Loaded::find(std::string_view name) const {
    for (const auto& [n, a] : exports) {
        if (n == name) {
            return a;
        }
    }
    return 0;
}

MapResult map(const Image& image, const Plan& plan, MemoryBackend& memory, TlsProvider* tls) {
    MapResult out;
    if (!plan.ok()) {
        out.diagnostic = plan.problems.front();
        return out;
    }
    bool tlsRelocations = false;
    for (const auto& rel : image.relocations) {
        tlsRelocations |= rel.type == kRelTlsDesc || rel.type == kRelTlsDtpMod64 || rel.type == kRelTlsDtpRel64;
    }
    if ((image.tls || tlsRelocations) && tls == nullptr) {
        out.diagnostic = fail(Error::TlsUnsupported, "uses thread-local storage and no TLS provider was given");
        return out;
    }
    std::string error;
    if ((image.tls || tlsRelocations) && !tls->prepare(error)) {
        out.diagnostic = fail(Error::MapFailed, "could not prepare the module's TLS: " + error);
        return out;
    }
    uint8_t* staging = nullptr;
    uint64_t runtimeBase = 0;
    const uint64_t align = std::max<uint64_t>(image.maxAlign, kPageSize);
    if (!memory.reserve(image.size, align, staging, runtimeBase, error) || staging == nullptr) {
        out.diagnostic = fail(Error::MapFailed, "could not reserve " + std::to_string(image.size) + " bytes: " + error);
        return out;
    }
    const uint64_t bias = runtimeBase - image.base;
    std::memset(staging, 0, image.size);
    uint32_t counterReads = 0;
    for (const auto& s : image.loads) {
        std::memcpy(staging + (s.vaddr - image.base), image.file.data() + s.offset, s.filesz);
        if ((s.perm & kPermX) != 0) {
            counterReads += redirect_counter_reads(staging + (s.vaddr - image.base), s.filesz);  // V1-rc7
        }
    }

    // S for every referenced symbol: the module's own definitions move with the image,
    // resolved imports are absolute, weak imports nothing defines are null.
    // A thread-local's value stays what the plan says: an offset in the module's block, or the accessor
    // of an imported one.
    std::vector<uint64_t> symbolValue(image.symbols.size(), 0);
    for (const auto& d : plan.imports) {
        if (d.symbol < symbolValue.size()) {
            symbolValue[d.symbol] = d.source == Source::Module && !d.tls ? bias + d.address : d.address;
        }
    }

    // TLS-lab: the module's template is registered before any relocation is written, and before any
    // code of it runs: a constructor may already touch a thread-local.
    uint64_t tlsModule = 0;
    if (image.tls) {
        tlsModule = tls->register_module(bias + image.tls->vaddr, *image.tls, error);
        if (tlsModule == 0) {
            out.diagnostic = fail(Error::MapFailed, "could not register the module's TLS: " + error);
            return out;
        }
    }
    // Any failure from here on leaves a registered template whose image will not be there: abandon it.
    struct AbandonOnFailure {
        TlsProvider* tls;
        uint64_t module;
        bool keep = false;
        ~AbandonOnFailure() {
            if (!keep && module != 0) {
                tls->abandon(module);
            }
        }
    } abandonOnFailure{tls, tlsModule};
    const auto tlsOffset = [&](const Relocation& rel) {
        return (rel.symbol != 0 ? symbolValue[rel.symbol] : 0) + static_cast<uint64_t>(rel.addend);
    };
    const auto isTlsImport = [&](const Relocation& rel) {
        return rel.symbol != 0 && !image.symbols[rel.symbol].defined();
    };

    for (const auto& rel : image.relocations) {
        uint64_t value = 0;
        switch (rel.type) {
        case kRelTlsDesc: {
            const auto words = isTlsImport(rel) ?
                                   tls->import_descriptor(symbolValue[rel.symbol], rel.addend) :
                                   tls->descriptor(tlsModule, tlsOffset(rel));
            const uint64_t pair[2] = {words.first, words.second};
            std::memcpy(staging + (rel.offset - image.base), pair, sizeof(pair));
            continue;
        }
        case kRelTlsDtpMod64:
            value = isTlsImport(rel) ? tls->import_module(symbolValue[rel.symbol]) : tlsModule;
            break;
        case kRelTlsDtpRel64:
            // For an import, S is the variable itself (offset 0 from what its accessor returns).
            value = isTlsImport(rel) ? static_cast<uint64_t>(rel.addend) : tlsOffset(rel);
            break;
        case kRelNone:
            continue;
        case kRelRelative:
            value = bias + static_cast<uint64_t>(rel.addend);
            break;
        case kRelAbs64:
        case kRelGlobDat:
        case kRelJumpSlot: {
            const uint64_t s = rel.symbol != 0 ? symbolValue[rel.symbol] : 0;
            value = s + static_cast<uint64_t>(rel.addend);
            break;
        }
        default:
            out.diagnostic = fail(Error::UnsupportedRelocation, relocation_name(rel.type));
            return out;
        }
        std::memcpy(staging + (rel.offset - image.base), &value, sizeof(value));
    }

    Loaded loaded;
    loaded.runtimeBase = runtimeBase;
    loaded.size = image.size;
    const AddressMap m{&image.loads};
    const auto entry = [&](uint64_t vaddr) -> std::optional<uint64_t> {
        uint64_t v = 0;
        std::memcpy(&v, staging + (vaddr - image.base), sizeof(v));
        if (v == 0 || v == ~0ull) {
            return std::nullopt;  // empty slots are skipped, as ld.so does
        }
        const uint64_t rel = v - bias;
        const Segment* seg = m.segment(rel, 4);
        if (seg == nullptr || !(seg->perm & kPermX)) {
            return 0;  // not code: refused below
        }
        return v;
    };
    const auto collect = [&](uint64_t array, uint64_t count) -> bool {
        for (uint64_t i = 0; i < count; ++i) {
            const auto v = entry(array + 8 * i);
            if (v && *v == 0) {
                return false;
            }
            if (v) {
                loaded.constructors.push_back(*v);
            }
        }
        return true;
    };
    if (!collect(image.preinitArray, image.preinitCount)) {
        out.diagnostic = fail(Error::BadInitArray, "a preinit_array entry does not point into code");
        return out;
    }
    if (image.initFunc != 0) {
        loaded.constructors.push_back(bias + image.initFunc);
    }
    if (!collect(image.initArray, image.initCount)) {
        out.diagnostic = fail(Error::BadInitArray, "an init_array entry does not point into code");
        return out;
    }
    for (const auto& sym : image.symbols) {
        // A thread-local has no address of its own to export (its value is an offset in a block).
        if (sym.defined() && sym.bind != kStbLocal && !sym.name.empty() && sym.type != kSttTls) {
            loaded.exports.emplace_back(std::string(sym.name),
                sym.shndx == kShnAbs ? sym.value : bias + sym.value);
        }
    }
    loaded.ehFrame = image.ehFrame != 0 ? bias + image.ehFrame : 0;
    loaded.tlsModule = tlsModule;
    loaded.counterReadsRedirected = counterReads;

    if (!memory.publish(protections(image), error)) {
        out.diagnostic = fail(Error::ProtectFailed, "could not publish the image: " + error);
        return out;
    }
    abandonOnFailure.keep = true;
    out.loaded = std::move(loaded);
    return out;
}

void run_constructors(const Loaded& loaded) {
    for (const uint64_t fn : loaded.constructors) {
        reinterpret_cast<void (*)()>(static_cast<uintptr_t>(fn))();
    }
}

namespace {
void json_string(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}
}  // namespace

std::string report_json(std::string_view label, std::string_view mode, const ParseResult& parsed,
    const Plan* plan, const Diagnostic* mapError) {
    std::string j = "{\"schema\":\"dusklight-aleks-native-mod-report\",\"schema_version\":1,\"file\":";
    json_string(j, label);
    j += ",\"mode\":";
    json_string(j, mode);
    const Diagnostic* first = nullptr;
    if (!parsed.image) {
        first = &parsed.diagnostic;
    } else if (plan != nullptr && !plan->ok()) {
        first = &plan->problems.front();
    } else if (mapError != nullptr && mapError->error != Error::None) {
        first = mapError;
    }
    j += ",\"ok\":";
    j += first == nullptr ? "true" : "false";
    j += ",\"error\":";
    json_string(j, first == nullptr ? "none" : error_code(first->error));
    j += ",\"message\":";
    json_string(j, first == nullptr ? "" : first->message);
    if (parsed.image) {
        const Image& im = *parsed.image;
        j += ",\"image\":{\"bytes\":" + std::to_string(im.size) + ",\"max_align\":" + std::to_string(im.maxAlign) +
             ",\"bind_now\":" + (im.bindNow ? "true" : "false") + ",\"gnu_hash\":" + (im.gnuHash ? "true" : "false") +
             ",\"sysv_hash\":" + (im.sysvHash ? "true" : "false") + ",\"relro\":" + (im.relro ? "true" : "false") +
             ",\"eh_frame\":" + (im.ehFrame != 0 ? "true" : "false");
        if (im.tls) {
            j += ",\"tls\":{\"filesz\":" + std::to_string(im.tls->filesz) + ",\"memsz\":" +
                 std::to_string(im.tls->memsz) + ",\"align\":" + std::to_string(im.tls->align) + "}";
        }
        j += ",\"segments\":[";
        for (size_t i = 0; i < im.loads.size(); ++i) {
            const auto& s = im.loads[i];
            std::string perm;
            perm += (s.perm & kPermR) ? 'R' : '-';
            perm += (s.perm & kPermW) ? 'W' : '-';
            perm += (s.perm & kPermX) ? 'X' : '-';
            j += (i ? "," : "") + std::string("{\"vaddr\":") + std::to_string(s.vaddr) + ",\"memsz\":" +
                 std::to_string(s.memsz) + ",\"filesz\":" + std::to_string(s.filesz) + ",\"align\":" +
                 std::to_string(s.align) + ",\"perm\":\"" + perm + "\"}";
        }
        j += "],\"constructors\":" + std::to_string(im.preinitCount + im.initCount + (im.initFunc ? 1 : 0)) +
             ",\"destructors\":" + std::to_string(im.finiCount) + ",\"needed\":[";
        for (size_t i = 0; i < im.needed.size(); ++i) {
            j += i ? "," : "";
            json_string(j, im.needed[i]);
        }
        j += "],\"symbols\":" + std::to_string(im.symbols.size()) + ",\"relocations\":" +
             std::to_string(im.relocations.size()) + "}";
    }
    if (plan != nullptr) {
        j += ",\"relocation_types\":{";
        for (size_t i = 0; i < plan->relocationCounts.size(); ++i) {
            j += i ? "," : "";
            json_string(j, relocation_name(plan->relocationCounts[i].first));
            j += ":" + std::to_string(plan->relocationCounts[i].second);
        }
        j += "},\"imports\":[";
        for (size_t i = 0; i < plan->imports.size(); ++i) {
            const auto& d = plan->imports[i];
            j += i ? "," : "";
            j += "{\"name\":";
            json_string(j, d.name);
            if (!d.version.empty()) {
                j += ",\"version\":";
                json_string(j, d.version);
            }
            j += ",\"source\":";
            json_string(j, source_name(d.source));
            if (!d.why.empty()) {
                j += ",\"why\":";
                json_string(j, d.why);
            }
            j += "}";
        }
        j += "],\"problems\":[";
        for (size_t i = 0; i < plan->problems.size(); ++i) {
            j += i ? "," : "";
            j += "{\"error\":";
            json_string(j, error_code(plan->problems[i].error));
            j += ",\"message\":";
            json_string(j, plan->problems[i].message);
            j += "}";
        }
        j += "],\"exports\":[";
        for (size_t i = 0; i < plan->exports.size(); ++i) {
            j += i ? "," : "";
            json_string(j, plan->exports[i].first);
        }
        j += "]";
    }
    j += "}";
    return j;
}

}  // namespace dusk::mods::elf
