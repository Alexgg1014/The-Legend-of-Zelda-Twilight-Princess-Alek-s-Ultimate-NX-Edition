// H38-c6: the console findings as the game compiles them (src/dusk/mods/hardware_findings.hpp over
// hardware_findings.inc), checked with the same cases switch_pipeline_test.py checks on h38_policy.py's reading of the
// same file. Everything is constexpr, so a wrong answer fails the compile.
#include "dusk/mods/hardware_findings.hpp"

#include <cstdio>

using namespace dusk::mods::hardware;

namespace {
constexpr std::string_view kTpClassic = "org.dusklight.tp_classic_buttons";
constexpr std::string_view kTpClassicSha = "be891f39df113617192a54e609b866a776de50f5eab220406c864eda76b822fd";

// the c7 table
static_assert(verdict("alexandre.cooper_hat.texture_selector", "1.1.0").status == Status::Tested);
static_assert(verdict("dev.automata.deferred_fog", "2.0.0").status == Status::Incompatible);
static_assert(verdict("dev.automata.deferred_fog", "2.0.0", "da70c3c2bf37bcc721423d5de3c0dbb740e6877820481358b9959119579a4e8b").status ==
              Status::Incompatible);
static_assert(verdict("dev.automata.deferred_fog", "2.0.1").status == Status::Unverified);  // a future version
static_assert(verdict("dev.automata.deferred_fog", "2.0.0").listed);
static_assert(verdict("com.agustinfuac.fairy_companion", "1.0.0").status == Status::Unverified);
static_assert(verdict("com.kite.kh2camera", "1.0.1").status == Status::Unverified);
static_assert(verdict(kTpClassic, "1.3.1").status == Status::Incompatible);

// exact id and version, and bytes when both sides know them
static_assert(verdict(kTpClassic, "1.3.1", kTpClassicSha).status == Status::Incompatible);
static_assert(verdict(kTpClassic, "1.3.1", "0000000000000000000000000000000000000000000000000000000000000000").status ==
              Status::Unverified);
static_assert(verdict(kTpClassic, "1.3.2").status == Status::Unverified && verdict(kTpClassic, "1.3.2").listed);
static_assert(verdict("dev.nobody.unlisted", "1.0.0").status == Status::Unverified);
static_assert(!verdict("dev.nobody.unlisted", "1.0.0").listed);

// what the browser and the update check refuse
static_assert(!catalog_block_reason(kTpClassic, "1.3.1", kTpClassicSha).empty());
static_assert(catalog_block_reason(kTpClassic, "1.3.2").empty());  // a future version is not blocked
static_assert(catalog_block_reason("alexandre.cooper_hat.texture_selector", "1.1.0").empty());
static_assert(!catalog_block_reason("dev.automata.deferred_fog", "2.0.0").empty());
static_assert(catalog_block_reason("dev.automata.deferred_fog", "2.0.1").empty());
static_assert(!catalog_block_reason("dev.twilitrealm.randomizer", "1.0.6").empty());       // any catalog copy
static_assert(!catalog_block_reason("dev.twilitrealm.randomizer", "9.0.0").empty());
static_assert(!catalog_block_reason("com.dusklight.twilit_essentials", "2.5.0").empty());
static_assert(catalog_block_reason("dev.albt.albw", "0.3.0").empty());  // not delivered: no variant to protect

static_assert(status_name(Status::Incompatible) == "incompatible" && status_name(Status::Unverified) == "unverified");
}  // namespace

int main() {
    std::printf("hardware_findings_test: %zu findings, %zu Switch variants; the c7 table, exact matching and the "
                "browser's refusals hold\n",
        std::size(detail::kFindings), std::size(detail::kSwitchVariants));
    return 0;
}
