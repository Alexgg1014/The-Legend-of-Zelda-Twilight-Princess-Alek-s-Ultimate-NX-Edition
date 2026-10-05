// H37-c4: the few SDL3 I/O functions platforms/switch/source/borealis_io_switch.cpp calls, over
// stdio, for switch_io_test.cpp on a POSIX host. Not SDL: just enough that the file layer's own
// code -- every branch of File, open, check, join, create_child, list -- runs and can be checked.
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

struct SDL_IOStream {
    std::FILE* file = nullptr;
    SDL_IOStatus status = SDL_IO_STATUS_READY;
};

static std::string g_error;

extern "C" {

SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode) {
    std::FILE* f = std::fopen(file, mode);
    if (f == nullptr) {
        g_error = std::strerror(errno);
        return nullptr;
    }
    return new SDL_IOStream{f, SDL_IO_STATUS_READY};
}

size_t SDL_ReadIO(SDL_IOStream* c, void* ptr, size_t size) {
    const size_t got = std::fread(ptr, 1, size, c->file);
    c->status = got < size && std::ferror(c->file) ? SDL_IO_STATUS_ERROR : SDL_IO_STATUS_READY;
    return got;
}

size_t SDL_WriteIO(SDL_IOStream* c, const void* ptr, size_t size) {
    const size_t put = std::fwrite(ptr, 1, size, c->file);
    if (put < size) {
        c->status = SDL_IO_STATUS_ERROR;
        g_error = std::strerror(errno);
    }
    return put;
}

Sint64 SDL_SeekIO(SDL_IOStream* c, Sint64 offset, SDL_IOWhence whence) {
    const int w = whence == SDL_IO_SEEK_SET ? SEEK_SET : whence == SDL_IO_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if (std::fseek(c->file, static_cast<long>(offset), w) != 0) {
        g_error = std::strerror(errno);
        return -1;
    }
    return std::ftell(c->file);
}

Sint64 SDL_GetIOSize(SDL_IOStream* c) {
    const long here = std::ftell(c->file);
    if (here < 0 || std::fseek(c->file, 0, SEEK_END) != 0) {
        return -1;
    }
    const long end = std::ftell(c->file);
    std::fseek(c->file, here, SEEK_SET);
    return end;
}

bool SDL_FlushIO(SDL_IOStream* c) { return std::fflush(c->file) == 0; }

bool SDL_CloseIO(SDL_IOStream* c) {
    const bool ok = std::fclose(c->file) == 0;
    delete c;
    return ok;
}

SDL_IOStatus SDL_GetIOStatus(SDL_IOStream* c) { return c->status; }

const char* SDL_GetError(void) { return g_error.c_str(); }
bool SDL_ClearError(void) {
    g_error.clear();
    return true;
}

}  // extern "C"
