#pragma once
// aleks2 (H37-c4): the services a native mod imports, read without running it, and whether this
// build can give them.
//
// A native mod lists its service imports as ModMetaImport records in the "modmeta" section of its
// library (sdk/include/mods/api.h). Until H37-c3 they were looked at only when the mod activated:
// Dry Run reported a mod "compatible" (symbols resolved, hooks found) and Execute then failed it with
// "Mod requires a service that is unavailable". The records are plain data -- id, major, minimum
// minor, the optional flag; the runtime-only slot pointer is not read -- so they can be read from the
// file, with nothing mapped and no mod code run, which is what Dry Run promises.
//
// Only the game's own services (dev.twilitrealm.dusklight.*) are judged here. A service another mod
// exports exists only once that mod has loaded; ModLoader::resolve_service_imports decides those.
//
// Pure: the host's services come from a lookup the caller passes (the module table in the game, a
// fake one in tests).
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::mods::elf {

struct ServiceImport {
    std::string id;
    uint16_t major = 0;
    uint16_t minMinor = 0;
    bool optional = false;
};

struct ServiceScan {
    std::vector<ServiceImport> imports;
    std::string error;  // empty: the section was read (or there is none)
    bool ok() const { return error.empty(); }
};

// Reads the ServiceImport records from an ELF64 little-endian library's "modmeta" section. A library
// without that section has no imports. Malformed records are an error, never a guess.
ServiceScan scan_service_imports(std::span<const uint8_t> file);

// One service the host offers, at one major version.
struct HostService {
    uint16_t major = 0;
    uint16_t minor = 0;
    bool available = true;  // false: registered on no build of this platform (no file picker...)
};

// Every major version the host knows for `id`; empty if it knows none.
using HostServiceLookup = std::function<std::vector<HostService>(std::string_view id)>;

enum class ServiceProblemKind : uint8_t {
    Unavailable,   // the host knows it and cannot offer it here
    Unknown,       // the host has no such service at any version
    NewerHost,     // the mod wants a newer major, or a newer minor, than this build has
    OlderMod,      // the mod was built for an older major than this build has
};

struct ServiceProblem {
    ServiceImport import;
    ServiceProblemKind kind = ServiceProblemKind::Unavailable;
    bool blocking = true;  // false: the import is optional, the mod loads without it
    std::string message;   // the wording ModLoader::resolve_service_imports uses
};

// Judges every import of the game's services. Optional imports that cannot be satisfied are listed
// with blocking = false.
std::vector<ServiceProblem> check_services(std::span<const ServiceImport> imports, const HostServiceLookup& host);

// The first blocking problem as one line, or empty if the mod may load.
std::string first_blocking(std::span<const ServiceProblem> problems);

// Adds the game-service verdicts to a Dry Run report (elf::report_json's object) as a "services"
// array, one entry per import of the game's services, and, when one blocks, turns the report's
// ok/error/message into "ok":false, "service_unavailable" and the line first_blocking gives.
std::string annotate_report(std::string report, std::span<const ServiceImport> imports,
    std::span<const ServiceProblem> problems);

}  // namespace dusk::mods::elf
