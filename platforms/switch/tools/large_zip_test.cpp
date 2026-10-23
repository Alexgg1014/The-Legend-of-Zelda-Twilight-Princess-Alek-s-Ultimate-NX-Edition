// aleks2 (rc18): a .dusk package opened the way the Switch opens it -- src/dusk/archive.cpp (miniz through
// borealis::io::RandomAccessFile, platforms/switch/source/borealis_io_switch.cpp) -- for every path given.
// Prints the size the IO layer reports, whether the ZIP opens, the entry count, and whether mod.json reads.
#include "dusk/archive.hpp"

#include <borealis/io.hpp>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>

#include <cstdio>
#include <exception>

// borealis_io_switch.cpp's SDL streams are not on the path under test (RandomAccessFile is POSIX): stubs, so the
// file links on the host as it is.
extern "C" {
bool SDLCALL SDL_ClearError(void) { return true; }
const char* SDLCALL SDL_GetError(void) { return "stub"; }
bool SDLCALL SDL_CloseIO(SDL_IOStream*) { return false; }
bool SDLCALL SDL_FlushIO(SDL_IOStream*) { return false; }
Sint64 SDLCALL SDL_GetIOSize(SDL_IOStream*) { return -1; }
SDL_IOStatus SDLCALL SDL_GetIOStatus(SDL_IOStream*) { return SDL_IO_STATUS_ERROR; }
SDL_IOStream* SDLCALL SDL_IOFromFile(const char*, const char*) { return nullptr; }
size_t SDLCALL SDL_ReadIO(SDL_IOStream*, void*, size_t) { return 0; }
Sint64 SDLCALL SDL_SeekIO(SDL_IOStream*, Sint64, SDL_IOWhence) { return -1; }
size_t SDLCALL SDL_WriteIO(SDL_IOStream*, const void*, size_t) { return 0; }
}

int main(int argc, char** argv) {
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        auto opened = borealis::io::RandomAccessFile::open(argv[i]);
        std::printf("%s: io size %llu\n", argv[i],
            static_cast<unsigned long long>(opened.status == borealis::io::Status::Ok ? opened.file.size() : 0));
        try {
            dusk::archive::ZipArchive zip{argv[i]};
            const auto names = zip.file_names();
            const auto manifest = zip.read_file("mod.json");
            // The last entry is stored furthest into the file (past 2 GiB in a zip64 pack): read it whole.
            const auto last = zip.read_file(names.back());
            std::printf("  opens: %zu entries, mod.json %zu bytes, last entry %s %zu bytes\n", names.size(),
                manifest.size(), names.back().c_str(), last.size());
        } catch (const std::exception& e) {
            std::printf("  FAIL: %s\n", e.what());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
