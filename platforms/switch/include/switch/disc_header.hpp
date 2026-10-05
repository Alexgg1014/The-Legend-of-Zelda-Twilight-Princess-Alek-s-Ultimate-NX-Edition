#pragma once
// What a disc image says about itself, read from the disc and not from the file.
//
// An .iso is its own disc, but an .rvz, .wia or .gcz is not: the first bytes of the
// file are the container's own header, and the disc's header is somewhere inside
// the compressed data. Asking "which game is this" therefore has to go through the
// container reader (disc_container.hpp) -- the same one the game reads the disc
// with -- and take the first 0x20 bytes of the *logical* disc. This file is that
// question and nothing else; it does not decode anything itself.
#include "switch/disc_container.hpp"

#include <cstdint>
#include <string>

namespace dusk::sw::disc {

enum class HeaderStatus {
    Ok,           ///< a disc, and the fields below are filled in
    Unreadable,   ///< the file would not open
    NotADisc,     ///< it opened, but is not any disc image this knows about
    Unsupported,  ///< a known container this build does not read (CISO, WBFS, NFS, TGC)
    Damaged,      ///< a container that is ours to read, but would not open or ended early
};

enum class Console { Unknown, GameCube, Wii };

struct LogicalHeader {
    HeaderStatus status = HeaderStatus::NotADisc;
    Format format = Format::Unknown;  ///< the container the file turned out to be
    Console console = Console::Unknown;
    std::string game_id;  ///< six characters, as printed on the disc
    std::uint8_t disc_number = 0;
    std::uint8_t revision = 0;
    /// For a person: why it is not Ok. Empty when it is.
    std::string message;
};

/// Reads the first 0x20 bytes of the disc behind `stream`, whatever container it is
/// in, and interprets them. Never reads more than the container needs to open.
LogicalHeader read_logical_header(const RawStream& stream);

/// The same over a file on disk, through plain stdio. This is what the console's
/// validator calls: it does not hash the image, it only asks the header.
LogicalHeader read_logical_header(const char* path);

}  // namespace dusk::sw::disc
