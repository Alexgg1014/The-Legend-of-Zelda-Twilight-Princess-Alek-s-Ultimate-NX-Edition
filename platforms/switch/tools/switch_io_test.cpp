// H37-c4: what the file service stands on, on the Switch platform layer: borealis::io as
// platforms/switch/source/borealis_io_switch.cpp implements it, and the file picker that is not
// there (borealis_file_select_switch.cpp). The Horizon SDL is replaced by sdl_stdio_shim.cpp, a few
// functions over stdio, so the file layer's own logic runs on a POSIX host (test-switch-io.sh).
//
//   io:      check, open (read, truncate, append), size, read, write, seek, flush, close,
//            read_all and write_all as the service composes them, list, join, create_child,
//            display_name, atomic_replace -- and the refusals (not found, unsafe names, writing to a
//            read-only file, a "scheme://" location, an existing child);
//   pickers: capabilities() empty, busy() false, open_file / open_folder / export_file each answer
//            "canceled" with a message and never "selected".
#include <borealis/file_select.hpp>
#include <borealis/io.hpp>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace io = borealis::io;
namespace fs = std::filesystem;
namespace sel = borealis::file_select;

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// The service's read_all: open, size, read it all.
bool read_all(const std::string& location, std::vector<std::byte>& out) {
    auto opened = io::open(location, io::File::Mode::Read);
    if (opened.status != io::Status::Ok) {
        return false;
    }
    out.assign(opened.file.size(), std::byte{0});
    return opened.file.read(out.data(), out.size()) == out.size();
}

// The service's write_all: truncate, write, close (which flushes).
bool write_all(const std::string& location, std::span<const std::byte> bytes) {
    auto opened = io::open(location, io::File::Mode::Truncate);
    if (opened.status != io::Status::Ok) {
        return false;
    }
    return opened.file.write(bytes) && opened.file.close();
}

std::vector<std::byte> bytes_of(const std::string& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}

std::string string_of(const std::vector<std::byte>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

void test_io(const fs::path& dir) {
    const std::string folder = dir.string();
    const std::string a = (dir / "a.bin").string();

    check(io::check(folder) == io::Status::Ok, "check: an existing folder is Ok");
    check(io::check(a) == io::Status::NotFound, "check: a missing file is NotFound");
    check(io::check("sdmc://x") == io::Status::Unsupported, "check: a scheme:// location is Unsupported");
    check(io::open(a, io::File::Mode::Read).status == io::Status::NotFound, "open: reading a missing file is NotFound");

    // write_all, then read_all
    const auto payload = bytes_of("seed data \x01\x02 and more");
    check(write_all(a, payload), "write_all creates and fills a file");
    std::vector<std::byte> back;
    check(read_all(a, back) && back == payload, "read_all returns exactly what write_all wrote");
    check(io::check(a) == io::Status::Ok, "check: the file now exists");

    // size / seek / read / the read-only refusal
    {
        auto opened = io::open(a, io::File::Mode::Read);
        check(opened.status == io::Status::Ok && opened.file && !opened.file.writable(), "open (read): a read-only handle");
        check(opened.file.size() == payload.size(), "size matches");
        check(opened.file.seek(5), "seek to an offset");
        char tail[8] = {};
        check(opened.file.read(tail, 4) == 4 && std::memcmp(tail, "data", 4) == 0, "read after seek gives the bytes at that offset");
        check(!opened.file.write(bytes_of("x")), "write on a read-only file fails");
        check(!opened.file.error().empty(), "and says why");
        check(opened.file.read(tail, 0) == 0, "reading 0 bytes reads 0");
        check(opened.file.seek(payload.size()) && opened.file.read(tail, 4) == 0, "reading at the end gives 0, not an error");
        check(opened.file.close(), "close");
        check(opened.file.close(), "closing twice is harmless");
        check(opened.file.read(tail, 4) == 0, "reading a closed file reads nothing");
    }
    // truncate again, then append and flush
    {
        auto opened = io::open(a, io::File::Mode::Truncate);
        check(opened.status == io::Status::Ok && opened.file.writable(), "open (truncate): writable");
        check(opened.file.size() == 0, "truncate empties the file");
        check(opened.file.write(bytes_of("one")), "write");
        check(opened.file.flush(), "flush");
        check(opened.file.close(), "close flushes");
    }
    {
        auto opened = io::open(a, io::File::Mode::Append);
        check(opened.status == io::Status::Ok && opened.file.writable(), "open (append): writable");
        check(opened.file.write(bytes_of("two")) && opened.file.close(), "append and close");
        std::vector<std::byte> all;
        check(read_all(a, all) && string_of(all) == "onetwo", "append kept the old contents and added to the end");
    }
    // a file larger than a block, read back through seek
    {
        std::string big(300000, 'x');
        for (size_t i = 0; i < big.size(); i += 4099) {
            big[i] = static_cast<char>('a' + (i / 4099) % 26);
        }
        check(write_all((dir / "big.bin").string(), bytes_of(big)), "a 300 kB write");
        std::vector<std::byte> all;
        check(read_all((dir / "big.bin").string(), all) && string_of(all) == big, "a 300 kB read back byte for byte");
    }

    // join, create_child, list, display_name
    {
        const auto joined = io::join(folder, "a.bin");
        check(joined.status == io::Status::Ok && fs::equivalent(joined.location, a), "join: an existing child");
        check(io::join(folder, "nope.bin").status == io::Status::NotFound, "join: a missing child is NotFound");
        check(io::join(folder, "../a.bin").status != io::Status::Ok, "join: a path that climbs out is refused");
        check(io::join(folder, "/etc/passwd").status != io::Status::Ok, "join: an absolute path is refused");
        const auto created = io::create_child(folder, "made.txt");
        check(created.status == io::Status::Ok && fs::exists(dir / "made.txt"), "create_child: a new empty file");
        check(io::create_child(folder, "made.txt").status == io::Status::AlreadyExists, "create_child: never replaces an existing child");
        check(io::create_child(folder, "../x").status != io::Status::Ok, "create_child: an unsafe name is refused");
        check(io::create_child(folder, "a/b").status != io::Status::Ok, "create_child: a name with a separator is refused");
        check(io::display_name(a) == "a.bin", "display_name: the file's name");
    }
    {
        fs::create_directory(dir / "sub");
        const auto listed = io::list(folder);
        check(listed.status == io::Status::Ok, "list: a folder lists");
        bool sawA = false, sawSub = false, subIsDir = false;
        for (const auto& e : listed.entries) {
            sawA |= e.name == "a.bin";
            if (e.name == "sub") {
                sawSub = true;
                subIsDir = e.isDirectory;
            }
        }
        check(sawA && sawSub && subIsDir, "list: files and folders, the folder flagged");
        check(io::list((dir / "missing").string()).status == io::Status::NotFound, "list: a missing folder is NotFound");
    }
    // atomic_replace over an existing file (Horizon's rename() refuses; the layer must not)
    {
        write_all((dir / "src.txt").string(), bytes_of("new"));
        write_all((dir / "dst.txt").string(), bytes_of("old"));
        std::string error;
        check(io::atomic_replace(dir / "src.txt", dir / "dst.txt", error), "atomic_replace over an existing file");
        std::vector<std::byte> all;
        check(read_all((dir / "dst.txt").string(), all) && string_of(all) == "new", "the destination has the new contents");
        check(!fs::exists(dir / "src.txt"), "the source is gone");
    }
}

void test_pickers() {
    const auto caps = sel::capabilities();
    check(!caps.canOpenFile && !caps.canOpenFolder && !caps.canExportFile, "capabilities(): no picker of any kind");
    check(!sel::busy(), "busy(): never");
    int answers = 0;
    const auto expectCanceled = [&](sel::Result r) {
        ++answers;
        check(r.status == sel::Status::Canceled, "the picker answers Canceled");
        check(r.locations.empty(), "and offers no location");
        check(!r.message.empty(), "with a message");
    };
    sel::open_file(sel::FileOptions{}, expectCanceled);
    sel::open_folder(sel::FolderOptions{}, expectCanceled);
    sel::export_file(sel::ExportOptions{}, expectCanceled);
    check(answers == 3, "each of the three pickers answers exactly once");
    sel::open_file(sel::FileOptions{}, nullptr);  // no callback: nothing to call, nothing to crash
    check(true, "a request without a callback is harmless");
}

}  // namespace

int main() {
    char pattern[] = "/tmp/switch-io-test-XXXXXX";
    if (mkdtemp(pattern) == nullptr) {
        std::printf("cannot make a temporary folder\n");
        return 2;
    }
    const fs::path dir = pattern;
    test_io(dir);
    test_pickers();
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("switch io: all checks passed\n");
    return 0;
}
