// H36: the host pieces luau_probe_test.cpp needs and does not test. Each one stands in
// for something the console build gets from elsewhere, and says what.
#include "dusk/mod_loader.hpp"
#include "dusk/mods/loader/loader.hpp"
#include "dusk/mods/log_buffer.hpp"
#include "dusk/mods/loader/native_module.hpp"

#include <borealis/log.hpp>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;

std::vector<std::string>& loader_log_lines() {
    static std::vector<std::string> lines;
    return lines;
}

// The loader's own log (dusk/mods/log_buffer.cpp keeps it for the Mods window).
namespace dusk::mods::log {
void emit(Source, const std::string& modId, LogLevel level, const std::string& message) {
    loader_log_lines().push_back("[" + std::to_string(static_cast<int>(level)) + "] " + modId +
                                 ": " + message);
}
}  // namespace dusk::mods::log

// borealis' logger (borealis/src/log.cpp writes to the console and the log file).
namespace borealis::log::detail {
bool enabled(LogLevel, std::string_view) noexcept {
    return true;
}
void write(LogLevel, std::string_view module, std::string_view text) {
    loader_log_lines().push_back(std::string(module) + ": " + std::string(text));
}
void write_fatal(std::string_view module, std::string_view text) {
    std::fprintf(stderr, "fatal %.*s: %.*s\n", static_cast<int>(module.size()), module.data(),
        static_cast<int>(text.size()), text.data());
    std::abort();
}
}  // namespace borealis::log::detail

// dusk/data.cpp: on the console these handle "sdmc:" paths, which a PC does not have.
namespace dusk::data {
std::filesystem::path absolute_path(const std::filesystem::path& path) {
    return std::filesystem::absolute(path);
}
std::string abbreviated_path_string(const std::filesystem::path& path) {
    return path.string();
}
}  // namespace dusk::data

// mods/loader/natives.cpp, which also holds the native loader; the same rule, verbatim.
namespace dusk::mods {
bool has_native_library_extension(std::string_view name) {
    const auto endsWith = [name](std::string_view extension) {
        if (name.size() < extension.size()) {
            return false;
        }
        const auto suffix = name.substr(name.size() - extension.size());
        return std::ranges::equal(suffix, extension, [](char lhs, char rhs) {
            const auto lower = [](char value) {
                return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) :
                                                      value;
            };
            return lower(lhs) == lower(rhs);
        });
    };
    return endsWith(".dll"sv) || endsWith(".so"sv) || endsWith(".dylib"sv);
}
}  // namespace dusk::mods

// mods/loader/context.cpp, whose other half (failing a mod) needs the whole ModLoader;
// the lookup, verbatim.
namespace dusk::mods {
LoadedMod* mod_from_context(ModContext* context) {
    return context != nullptr ? context->mod : nullptr;
}
const LoadedMod* mod_from_context(const ModContext* context) {
    return context != nullptr ? context->mod : nullptr;
}
const char* mod_id_from_context(ModContext* context) {
    const auto* mod = mod_from_context(context);
    return mod != nullptr ? mod->metadata.id.c_str() : "mod";
}
}  // namespace dusk::mods

// dusk/config.cpp defines this (the key function, so the vtable lives there). LoadedMod
// holds its enable flag through one; this test never creates any.
dusk::config::ConfigVarBase::~ConfigVarBase() = default;

// LoadedMod owns one through NativeMod; this test never creates one (the runtime is linked
// in and wired directly), it only has to be destructible. The Switch build's is the same.
dusk::mods::loader::NativeModule::~NativeModule() = default;

// SDL3's file streams over stdio, for borealis_io_switch.cpp. The console uses SDL's own
// (devkitPro's backend); a PC with no SDL3 installed gets these.
struct SDL_IOStream {
    FILE* file;
    SDL_IOStatus status;
};

namespace {
thread_local std::string g_sdlError;
}

extern "C" {
const char* SDL_GetError(void) {
    return g_sdlError.c_str();
}
bool SDL_ClearError(void) {
    g_sdlError.clear();
    return true;
}
SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode) {
    FILE* handle = std::fopen(file, mode);
    if (handle == nullptr) {
        g_sdlError = std::string("cannot open ") + file;
        return nullptr;
    }
    return new SDL_IOStream{handle, SDL_IO_STATUS_READY};
}
bool SDL_CloseIO(SDL_IOStream* context) {
    if (context == nullptr) {
        return false;
    }
    const bool ok = std::fclose(context->file) == 0;
    delete context;
    return ok;
}
Sint64 SDL_GetIOSize(SDL_IOStream* context) {
    const long at = std::ftell(context->file);
    std::fseek(context->file, 0, SEEK_END);
    const long size = std::ftell(context->file);
    std::fseek(context->file, at, SEEK_SET);
    return size;
}
Sint64 SDL_SeekIO(SDL_IOStream* context, Sint64 offset, SDL_IOWhence whence) {
    const int origin = whence == SDL_IO_SEEK_SET ? SEEK_SET :
                       whence == SDL_IO_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if (std::fseek(context->file, static_cast<long>(offset), origin) != 0) {
        g_sdlError = "seek failed";
        return -1;
    }
    return std::ftell(context->file);
}
size_t SDL_ReadIO(SDL_IOStream* context, void* ptr, size_t size) {
    const size_t read = std::fread(ptr, 1, size, context->file);
    context->status = read < size ? (std::feof(context->file) ? SDL_IO_STATUS_EOF :
                                                                 SDL_IO_STATUS_ERROR) :
                                    SDL_IO_STATUS_READY;
    return read;
}
size_t SDL_WriteIO(SDL_IOStream* context, const void* ptr, size_t size) {
    const size_t written = std::fwrite(ptr, 1, size, context->file);
    context->status = written < size ? SDL_IO_STATUS_ERROR : SDL_IO_STATUS_READY;
    return written;
}
bool SDL_FlushIO(SDL_IOStream* context) {
    return std::fflush(context->file) == 0;
}
SDL_IOStatus SDL_GetIOStatus(SDL_IOStream* context) {
    return context->status;
}
}
