// aleks2 (DM2-b): std::basic_filebuf / basic_ifstream / basic_ofstream / basic_fstream<char> laid out
// as a Linux-built module expects them, for linux-aarch64 mods only.
//
// Measured (research/plan-mods-100/iolayout): of everything in libstdc++'s streams, only basic_filebuf
// differs between Ubuntu's GCC 13 + glibc and devkitA64 + newlib. Its _M_lock is a __c_lock, which is
// glibc's 48-byte pthread_mutex_t there and newlib's 12-byte one here, so _M_file and every member
// after it sit 32 bytes further on Linux (filebuf 248 vs 216 bytes; ifstream 528, ofstream 520,
// fstream 536). libstdc++ never touches _M_lock. A module's inline code (is_open(), the constructors'
// member initialisation, the virtual-base offsets) knows the Linux layout; the game's library code
// the other. H37-c5's Randomizer died in basic_ofstream's constructor for exactly that.
//
// So this unit is compiled against a copy of the toolchain's <fstream> whose basic_filebuf pads
// _M_lock to 48 bytes (cmake/LinuxStreams.cmake makes it; the __c_lock type itself is untouched, as
// __basic_file's constructor -- in the game's libstdc++ -- is mangled with it), and instantiates the
// four classes here. Every symbol of those classes this object defines is then renamed to
// aleks2lx_<name> (objcopy --redefine-syms, platforms/switch/tools/linux_streams_rename.py), so the
// game keeps its own instances, and the loader binds a Linux module's imports of the original names
// to the renamed ones (elf_runtime_policy.cpp, linux_stream_shims()). What they call out of line --
// basic_streambuf, basic_ios, ios_base, locale, __basic_file, codecvt -- is the game's: identical
// layout, measured.
//
// Not covered: wchar_t streams (no published mod imports them; still refused), and dynamic_cast or
// typeid across the two layouts: a module's ifstream has the renamed type_info, so a dynamic_cast to
// std::ifstream inside the game's code would fail on it (nothing in the game does one).
#include <fstream>

#ifndef ALEKS2_LINUX_FILEBUF_LAYOUT
#error "compiled without the glibc-layout <fstream> copy (cmake/LinuxStreams.cmake)"
#endif

static_assert(sizeof(std::basic_filebuf<char>) == 248, "basic_filebuf<char> is not glibc's size");
static_assert(sizeof(std::basic_ifstream<char>) == 528, "basic_ifstream<char> is not glibc's size");
static_assert(sizeof(std::basic_ofstream<char>) == 520, "basic_ofstream<char> is not glibc's size");
static_assert(sizeof(std::basic_fstream<char>) == 536, "basic_fstream<char> is not glibc's size");

namespace std {
template class basic_filebuf<char>;
template class basic_ifstream<char>;
template class basic_ofstream<char>;
template class basic_fstream<char>;
}  // namespace std

// Where _M_file landed, for the self-check in linux_streams_check(): glibc puts it at 112.
namespace {
struct FilebufProbe : std::basic_filebuf<char> {
    static size_t file_offset() {
        FilebufProbe* p = nullptr;
        return reinterpret_cast<size_t>(&p->_M_file);
    }
};
}  // namespace

extern "C" size_t aleks2lx_filebuf_file_offset() {
    return FilebufProbe::file_offset();
}
