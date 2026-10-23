// aleks2 (rc17, Wii discs): AES-128-CBC through libnx (the CPU's AES instructions). See wii_crypto.hpp.
#include "switch/wii_crypto.hpp"

#include <switch.h>

namespace dusk::sw::disc {

void aes128_cbc_decrypt(const std::uint8_t key[16], const std::uint8_t iv[16], const void* in, void* out,
    std::size_t size) {
    Aes128CbcContext ctx;
    aes128CbcContextCreate(&ctx, key, iv, false);
    aes128CbcDecrypt(&ctx, out, in, size);
}

}  // namespace dusk::sw::disc
