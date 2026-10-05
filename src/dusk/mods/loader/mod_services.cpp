#include "mod_services.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dusk::mods::elf {
namespace {

constexpr std::string_view kHostPrefix = "dev.twilitrealm.dusklight.";

// ModMetaKind / record layout, sdk/include/mods/api.h.
constexpr uint8_t kMetaImport = 2;
constexpr uint8_t kImportOptional = 1u << 0u;
constexpr size_t kRecordHeader = 4;      // size u16, kind u8, flags u8
constexpr size_t kImportMajor = 4;       // u16
constexpr size_t kImportMinMinor = 6;    // u16
constexpr size_t kImportSlot = 8;        // 8 bytes, runtime only
constexpr size_t kImportId = 16;         // char[64]
constexpr size_t kServiceIdSize = 64;
constexpr size_t kImportSize = kImportId + kServiceIdSize;

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return u16(p) | (static_cast<uint32_t>(u16(p + 2)) << 16); }
uint64_t u64(const uint8_t* p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }

bool fits(uint64_t offset, uint64_t size, size_t total) {
    return offset <= total && size <= total - offset;
}

}  // namespace

ServiceScan scan_service_imports(std::span<const uint8_t> file) {
    ServiceScan scan;
    const auto fail = [&](std::string why) {
        scan.imports.clear();
        scan.error = std::move(why);
        return scan;
    };
    if (file.size() < 64 || std::memcmp(file.data(), "\x7f" "ELF", 4) != 0 || file[4] != 2 || file[5] != 1) {
        return fail("not an ELF64 little-endian library");
    }
    const uint64_t shoff = u64(&file[0x28]);
    const uint16_t shentsize = u16(&file[0x3A]);
    const uint16_t shnum = u16(&file[0x3C]);
    const uint16_t shstrndx = u16(&file[0x3E]);
    if (shnum == 0 || shoff == 0) {
        return scan;  // no section headers: no modmeta section to read
    }
    if (shentsize < 64 || !fits(shoff, static_cast<uint64_t>(shentsize) * shnum, file.size()) || shstrndx >= shnum) {
        return fail("malformed section header table");
    }
    const auto section = [&](uint16_t index) { return file.data() + shoff + static_cast<uint64_t>(index) * shentsize; };
    const uint8_t* names = section(shstrndx);
    const uint64_t namesOffset = u64(names + 0x18);
    const uint64_t namesSize = u64(names + 0x20);
    if (!fits(namesOffset, namesSize, file.size())) {
        return fail("malformed section name table");
    }

    for (uint16_t i = 0; i < shnum; ++i) {
        const uint8_t* header = section(i);
        const uint32_t nameIndex = u32(header);
        if (nameIndex >= namesSize) {
            continue;
        }
        const char* name = reinterpret_cast<const char*>(file.data() + namesOffset + nameIndex);
        const size_t room = static_cast<size_t>(namesSize - nameIndex);
        if (strnlen(name, room) != 7 || std::memcmp(name, "modmeta", 7) != 0) {
            continue;
        }
        const uint32_t type = u32(header + 4);
        if (type == 8) {  // SHT_NOBITS: nothing in the file
            return fail("the modmeta section has no contents");
        }
        const uint64_t offset = u64(header + 0x18);
        const uint64_t size = u64(header + 0x20);
        if (!fits(offset, size, file.size())) {
            return fail("the modmeta section lies outside the file");
        }
        const uint8_t* base = file.data() + offset;
        uint64_t at = 0;
        while (at < size) {
            if (size - at < kRecordHeader) {
                return fail("a modmeta record is cut off");
            }
            const uint16_t recordSize = u16(base + at);
            const uint8_t kind = base[at + 2];
            const uint8_t flags = base[at + 3];
            if (recordSize < kRecordHeader || recordSize % 8 != 0 || recordSize > size - at) {
                return fail("a modmeta record has an invalid size");
            }
            if (kind == kMetaImport) {
                if (recordSize != kImportSize) {
                    return fail("a service import record has the wrong size");
                }
                const char* id = reinterpret_cast<const char*>(base + at + kImportId);
                const size_t length = strnlen(id, kServiceIdSize);
                if (length == kServiceIdSize || length == 0) {
                    return fail("a service import has an unterminated or empty id");
                }
                ServiceImport import;
                import.id.assign(id, length);
                import.major = u16(base + at + kImportMajor);
                import.minMinor = u16(base + at + kImportMinMinor);
                import.optional = (flags & kImportOptional) != 0;
                scan.imports.push_back(std::move(import));
            }
            at += recordSize;
        }
        return scan;
    }
    return scan;  // no modmeta section: no imports
}

std::vector<ServiceProblem> check_services(std::span<const ServiceImport> imports, const HostServiceLookup& host) {
    std::vector<ServiceProblem> problems;
    for (const auto& import : imports) {
        if (!std::string_view{import.id}.starts_with(kHostPrefix)) {
            continue;  // another mod's service: resolved when that mod has loaded
        }
        ServiceProblem problem;
        problem.import = import;
        problem.blocking = !import.optional;
        const std::vector<HostService> known = host(import.id);
        const auto same = std::ranges::find_if(known, [&](const HostService& s) { return s.major == import.major; });
        const char* wording = "Mod requires a service that is unavailable";
        if (same != known.end()) {
            if (!same->available) {
                problem.kind = ServiceProblemKind::Unavailable;
            } else if (same->minor < import.minMinor) {
                problem.kind = ServiceProblemKind::NewerHost;
                wording = "Mod requires a newer Dusklight version";
            } else {
                continue;  // satisfied
            }
        } else if (known.empty()) {
            problem.kind = ServiceProblemKind::Unknown;
        } else {
            uint16_t highest = 0;
            for (const auto& s : known) {
                highest = std::max(highest, s.major);
            }
            if (import.major > highest) {
                problem.kind = ServiceProblemKind::NewerHost;
                wording = "Mod requires a newer Dusklight version";
            } else {
                problem.kind = ServiceProblemKind::OlderMod;
                wording = "Mod must be updated for the current Dusklight version";
            }
        }
        problem.message = std::string(wording) + " (missing: " + import.id + ")";
        problems.push_back(std::move(problem));
    }
    return problems;
}

namespace {

void json_escape(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += ' ';
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

const char* kind_name(ServiceProblemKind kind) {
    switch (kind) {
    case ServiceProblemKind::Unavailable: return "unavailable";
    case ServiceProblemKind::Unknown: return "unknown";
    case ServiceProblemKind::NewerHost: return "needs_newer_dusklight";
    case ServiceProblemKind::OlderMod: return "mod_needs_update";
    }
    return "unavailable";
}

}  // namespace

std::string annotate_report(std::string report, std::span<const ServiceImport> imports,
    std::span<const ServiceProblem> problems) {
    std::string services = ",\"services\":[";
    bool first = true;
    for (const auto& import : imports) {
        if (!std::string_view{import.id}.starts_with(kHostPrefix)) {
            continue;
        }
        const auto problem = std::ranges::find_if(
            problems, [&](const ServiceProblem& p) { return p.import.id == import.id && p.import.major == import.major; });
        services += first ? "" : ",";
        first = false;
        services += "{\"id\":";
        json_escape(services, import.id);
        services += ",\"major\":" + std::to_string(import.major) + ",\"min_minor\":" +
                    std::to_string(import.minMinor) + ",\"optional\":" + (import.optional ? "true" : "false") +
                    ",\"status\":";
        json_escape(services, problem == problems.end() ? "ok" : kind_name(problem->kind));
        services += "}";
    }
    services += "]";
    const size_t end = report.rfind('}');
    if (end == std::string::npos) {
        return report;
    }
    report.insert(end, services);
    const std::string blocking = first_blocking(problems);
    if (!blocking.empty()) {
        static constexpr std::string_view ok = "\"ok\":true,\"error\":\"none\",\"message\":\"\"";
        const size_t at = report.find(ok);
        if (at != std::string::npos) {
            std::string replacement = "\"ok\":false,\"error\":\"service_unavailable\",\"message\":";
            json_escape(replacement, blocking);
            report.replace(at, ok.size(), replacement);
        }
    }
    return report;
}

std::string first_blocking(std::span<const ServiceProblem> problems) {
    std::string line;
    size_t blocking = 0;
    for (const auto& problem : problems) {
        if (!problem.blocking) {
            continue;
        }
        if (blocking++ == 0) {
            line = problem.message;
        }
    }
    if (blocking > 1) {
        line += " (and " + std::to_string(blocking - 1) + " more service problem(s))";
    }
    return line;
}

}  // namespace dusk::mods::elf
