#pragma once
// aleks2 (rc17, Wii discs): the two things a Wii disc needs that a GameCube disc does not -- AES and a key.
//
// AES-128-CBC decryption is the only primitive: a Wii partition is encrypted per 0x8000-byte cluster, and its
// title key is itself encrypted with the console's common key. On the console it is libnx's hardware AES
// (wii_crypto_switch.cpp); the host tests link their own (platforms/switch/tools/wii_aes_host.cpp).
//
// The common key is never part of this program. Only an encrypted (.iso) image needs it -- RVZ and WIA store the
// partition decrypted, with its title key in the clear -- and then it is read from a file the player provides from
// their own Wii: BootMii's keys.bin, or the 16 bytes on their own (binary or hex).
#include <cstddef>
#include <cstdint>
#include <string>

namespace dusk::sw::disc {

/// AES-128-CBC decryption of `size` bytes (a multiple of 16). `in` and `out` must not overlap.
void aes128_cbc_decrypt(const std::uint8_t key[16], const std::uint8_t iv[16], const void* in, void* out,
    std::size_t size);

/// The file names looked for, in order, in the data directory:
///   wii_common_key.bin  the 16 bytes
///   wii_common_key.txt  32 hex digits (spaces, colons and a 0x prefix allowed)
///   keys.bin            BootMii's dump of the console's OTP (0x400 bytes; the common key at 0x114)
inline constexpr const char* kWiiKeyFiles[] = {"wii_common_key.bin", "wii_common_key.txt", "keys.bin"};

/// Reads the common key from `dir`. On success fills `key`, names the file in `source` and returns true. A file
/// that exists but is not one of the shapes above is reported in `problem` (and the next one is tried); nothing
/// found leaves `problem` empty.
bool load_wii_common_key(const std::string& dir, std::uint8_t key[16], std::string& source, std::string& problem);

}  // namespace dusk::sw::disc
