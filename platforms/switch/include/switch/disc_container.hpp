#pragma once
// aleks2: compressed disc images, so that "it does not find my disc" stops
// being the first thing a normal person hits.
//
// Dolphin has defaulted to RVZ for years. Somebody who dumped their own copy
// the ordinary way has an .rvz, not an .iso, and the reader this port shipped
// with read uncompressed images only -- so the game found nothing, said
// nothing useful, and the person had no way to know that the fix was a menu
// item in a different program.
//
// This sits under the disc reader as a translation layer: it turns a file in
// some container into the flat sequence of bytes a GameCube disc is, and
// everything above it -- the FST, the file reads, Aurora's DVD layer -- goes
// on working unchanged.
//
// What is implemented, and why those:
//
//   raw   .iso/.gcm    the file is already the disc
//   RVZ   .rvz         Dolphin's default since 5.0-12188
//   WIA   .wia         RVZ's predecessor, same container
//   GCZ   .gcz         what Dolphin defaulted to before that
//
// What is deliberately not: CISO, WBFS, NFS and TGC. The first two are Wii
// formats (a GameCube game is never in one), NFS is the Wii U Virtual Console
// layout, and TGC is an embedded mini-disc. They are still *detected*, because
// telling somebody "this is a Wii format and this is a GameCube game" is worth
// far more than failing to open it.
//
// The correctness of this file was established against a real 1.46 GB image
// and DolphinTool's own output, byte for byte, on the host -- see
// aleks2/tools/disc-ref.py, which is the reference the C++ was checked
// against, and aleks2/testkit/disc-container-test.cpp.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dusk::sw::disc {

/// How the container reads the file underneath it. Deliberately a pair of
/// function pointers rather than a path: on this console the file is already
/// open as an Aurora stream by the time we get here, and on the host the test
/// harness hands it a plain FILE*.
struct RawStream {
    void* user = nullptr;
    std::int64_t (*read_at)(void* user, std::uint64_t offset, void* out, std::size_t length) = nullptr;
    std::uint64_t (*length)(void* user) = nullptr;
};

enum class Format {
    Unknown,
    Raw,   ///< .iso / .gcm -- no container at all
    Rvz,
    Wia,
    Gcz,
    Ciso,  ///< detected, not supported
    Wbfs,  ///< detected, not supported
    Nfs,   ///< detected, not supported
    Tgc,   ///< detected, not supported
};

const char* format_name(Format format);

/// True for the ones this file can actually read.
bool format_supported(Format format);

/// A sentence for a person, not a log line: what the file is, and what to do
/// about it if we cannot read it.
std::string format_advice(Format format);

class Container {
public:
    virtual ~Container() = default;

    /// Reads `length` bytes from logical disc offset `offset`. Returns false
    /// if any of it could not be produced; partial output is not reported as
    /// success, because a disc read that half works is worse than one that
    /// fails.
    virtual bool read(std::uint64_t offset, void* out, std::size_t length) = 0;

    /// The size of the disc this represents, not of the file.
    virtual std::uint64_t size() const = 0;

    virtual Format format() const = 0;
};

/// Sniffs the container from the first bytes of the stream. Never reads more
/// than a header, so it is cheap enough to call before deciding anything.
Format detect(const RawStream& stream);

struct OpenResult {
    std::unique_ptr<Container> container;  ///< null unless error is empty
    Format format = Format::Unknown;
    std::string error;
};

OpenResult open(const RawStream& stream);

}  // namespace dusk::sw::disc
