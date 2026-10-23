// aleks2 (rc18): src/dusk/file_copy.hpp. fchmod here fails the way libnx's sdmc device does (fsdev_fchmod: ENOSYS),
// which reproduces the console's "Export Failed: Function not implemented" on the host: std::filesystem::copy_file
// writes the file and still fails; copy_file_stdio does not need fchmod.
#include "dusk/file_copy.hpp"
#include "dusk/utilities.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern "C" int fchmod(int, mode_t) {
    errno = ENOSYS;
    return -1;
}

namespace fs = std::filesystem;
static int s_failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++s_failures;
    }
}

static std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static void write_all(const fs::path& p, const std::string& data) {
    std::ofstream(p, std::ios::binary) << data;
}

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "dusk-file-copy-test";
    fs::remove_all(root);
    fs::create_directories(root / "exports");
    std::string big(200 * 1024 + 7, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);
    write_all(root / "artifact-0.gci", big);

    // The console's failure, reproduced: the bytes are copied, the call still fails (ENOSYS from fchmod).
    std::error_code ec;
    const bool stdOk = fs::copy_file(root / "artifact-0.gci", root / "exports" / "std.gci",
        fs::copy_options::overwrite_existing, ec);
    check(!stdOk && ec.value() == ENOSYS, "std::filesystem::copy_file fails with ENOSYS when fchmod is missing");
    std::printf("std::filesystem::copy_file with libnx's fchmod: %s (%s)\n", stdOk ? "ok" : "failed",
        ec.message().c_str());

    // The replacement.
    check(dusk::file_copy::copy_file_stdio(root / "artifact-0.gci", root / "exports" / "a.gci", ec) && !ec,
        "copy_file_stdio copies");
    check(read_all(root / "exports" / "a.gci") == big, "copy is byte-identical (200 KiB, several chunks)");
    write_all(root / "exports" / "b.gci", std::string(500000, 'x'));
    check(dusk::file_copy::copy_file_stdio(root / "artifact-0.gci", root / "exports" / "b.gci", ec),
        "overwrites an existing, longer file");
    check(read_all(root / "exports" / "b.gci") == big, "overwrite leaves no tail of the old file");
    write_all(root / "empty.gci", "");
    check(dusk::file_copy::copy_file_stdio(root / "empty.gci", root / "exports" / "e.gci", ec) &&
              fs::file_size(root / "exports" / "e.gci") == 0,
        "empty file");
    check(!dusk::file_copy::copy_file_stdio(root / "missing.gci", root / "exports" / "m.gci", ec) &&
              ec.value() == ENOENT && !fs::exists(root / "exports" / "m.gci"),
        "missing source: ENOENT, nothing created");
    check(!dusk::file_copy::copy_file_stdio(root / "artifact-0.gci", root / "no-such-dir" / "x.gci", ec) &&
              ec.value() == ENOENT,
        "missing destination folder: ENOENT");

    // The export name: only [A-Za-z0-9._-].
    const std::string name = dusk::utils::safe_filename("01-GZ2E-gczelda2: Link?\"<>|*-2026-10-21_101500.gci");
    check(name == "01-GZ2E-gczelda2__Link______-2026-10-21_101500.gci", "export name sanitized");

    fs::remove_all(root);
    if (s_failures == 0) std::printf("file_copy_test: all checks passed\n");
    return s_failures == 0 ? 0 : 1;
}
