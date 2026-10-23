// aleks2 (V1-rc7): redirect_counter_reads (src/dusk/mods/loader/elf_loader.hpp): every MRS Xt, CNTVCT_EL0 becomes
// MRS Xt, CNTPCT_EL0 with the same register; nothing else changes. The words are Twilit Essentials 2.8.4's
// (+0x111f34 mrs x8, cntfrq_el0 / +0x111f5c mrs x8, cntvct_el0) and neighbours.
#include "dusk/mods/loader/elf_loader.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    };
    using dusk::mods::elf::redirect_counter_reads;
    std::vector<uint32_t> words = {
        0xd53be008u,  // mrs x8, cntfrq_el0: kept
        0xd53be048u,  // mrs x8, cntvct_el0 -> mrs x8, cntpct_el0
        0xd53be040u,  // mrs x0, cntvct_el0
        0xd53be05eu,  // mrs x30, cntvct_el0
        0xd53be028u,  // mrs x8, cntpct_el0: already fine
        0xf90037e8u,  // str x8, [sp, #104]
        0xd53be0c8u,  // mrs x8, cntvctss_el0: not this encoding, kept
    };
    const auto before = words;
    const uint32_t n = redirect_counter_reads(reinterpret_cast<uint8_t*>(words.data()), words.size() * 4);
    check(n == 3, "three cntvct_el0 reads");
    check(words[0] == before[0], "cntfrq kept");
    check(words[1] == 0xd53be028u, "x8 redirected");
    check(words[2] == 0xd53be020u, "x0 redirected");
    check(words[3] == 0xd53be03eu, "x30 redirected");
    check(words[4] == before[4] && words[5] == before[5] && words[6] == before[6], "others untouched");
    // a size that is not a multiple of 4 never reads past it
    uint8_t tail[6] = {0x48, 0xe0, 0x3b, 0xd5, 0x48, 0xe0};
    check(redirect_counter_reads(tail, sizeof tail) == 1, "only whole words");
    if (failures == 0) {
        std::printf("elf counter reads: cntvct_el0 -> cntpct_el0, same register, nothing else touched: ok\n");
    }
    return failures == 0 ? 0 : 1;
}
