// aleks2 (rc17, Wii discs): the host tests' AES-128-CBC (OpenSSL), standing in for wii_crypto_switch.cpp.
#include "switch/wii_crypto.hpp"

#include <openssl/evp.h>

#include <cstdlib>

namespace dusk::sw::disc {

void aes128_cbc_decrypt(const std::uint8_t key[16], const std::uint8_t iv[16], const void* in, void* out,
    std::size_t size) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    if (ctx == nullptr || EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key, iv) != 1 ||
        EVP_CIPHER_CTX_set_padding(ctx, 0) != 1 ||
        EVP_DecryptUpdate(ctx, static_cast<unsigned char*>(out), &len, static_cast<const unsigned char*>(in),
            static_cast<int>(size)) != 1) {
        std::abort();
    }
    EVP_CIPHER_CTX_free(ctx);
}

}  // namespace dusk::sw::disc
