// aleks2 (SHADERS-c1) host test for src/dusk/shader_warmup.cpp: when the preparation
// screen shows and why, the marker round trip (and what makes it unreadable), the time
// estimate, and the lab options.
#include "dusk/shader_warmup.hpp"

#include <iostream>
#include <string>

namespace sw = dusk::shader_warmup;

static int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

namespace {

sw::Key sample() {
    sw::Key k;
    k.driver = "NVIDIA Tegra X1 (NVK gm20b)|NVK: Mesa 26.2.3";
    k.pipelineSchema = 1;
    k.gxVersion = 13;
    k.clearVersion = 5;
    k.rmlVersion = 3;
    k.dawnSchema = 2;
    k.listSignature = 0x1234abcd5678ef00ull;
    k.layoutKey = 0xfeedfacecafebeefull;
    k.runtimeIdentity = 0x0123456789abcdefull;
    return k;
}

void test_decide() {
    const auto now = sample();
    CHECK(sw::decide(std::nullopt, now, false) == sw::Reason::FirstLaunch);
    CHECK(sw::decide(now, now, false) == sw::Reason::None);
    CHECK(sw::decide(now, now, true) == sw::Reason::CacheRepaired);

    auto m = now;
    m.driver = "NVIDIA Tegra X1 (NVK gm20b)|NVK: Mesa 26.2.2";
    CHECK(sw::decide(m, now, false) == sw::Reason::DriverChanged);
    m = now;
    m.gxVersion = 12;
    CHECK(sw::decide(m, now, false) == sw::Reason::SchemaChanged);
    m = now;
    m.dawnSchema = 1;
    CHECK(sw::decide(m, now, false) == sw::Reason::SchemaChanged);
    m = now;
    m.runtimeIdentity ^= 1;  // a build that changes shader generation, same list
    CHECK(sw::decide(m, now, false) == sw::Reason::RuntimeChanged);
    m = now;
    m.listSignature = 0;
    CHECK(sw::decide(m, now, false) == sw::Reason::ListChanged);
    m = now;
    m.layoutKey ^= 1;
    CHECK(sw::decide(m, now, false) == sw::Reason::RenderChanged);
    // The most fundamental change wins the message.
    m = now;
    m.driver = "other";
    m.layoutKey ^= 1;
    CHECK(sw::decide(m, now, true) == sw::Reason::DriverChanged);

    for (auto r : {sw::Reason::FirstLaunch, sw::Reason::DriverChanged, sw::Reason::SchemaChanged, sw::Reason::RuntimeChanged,
                   sw::Reason::ListChanged, sw::Reason::RenderChanged, sw::Reason::CacheRepaired}) {
        CHECK(std::string{sw::reason_text(r)}.size() > 10);
        CHECK(std::string{sw::reason_name(r)} != "unknown");
    }
}

void test_marker() {
    const auto k = sample();
    const std::string text = sw::marker_text(k, 1760000000, 271.4, 1754);
    const auto back = sw::parse_marker(text);
    CHECK(back.has_value() && *back == k);

    // CRLF (edited on a PC) and extra lines are fine.
    std::string crlf;
    for (char c : text) {
        if (c == '\n') {
            crlf += "\r\n";
        } else {
            crlf += c;
        }
    }
    crlf += "note=something new\r\n";
    CHECK(sw::parse_marker(crlf).has_value());

    // Anything incomplete or malformed reads as "no marker": the screen shows again.
    CHECK(!sw::parse_marker("").has_value());
    CHECK(!sw::parse_marker(text.substr(0, text.size() / 2)).has_value());  // cut short
    std::string badNumber = text;
    badNumber.replace(badNumber.find("gx_version=13"), 13, "gx_version=1x");
    CHECK(!sw::parse_marker(badNumber).has_value());
    // A SHADERS-c1 marker (version 1, no runtime identity) is not trusted: the screen shows once.
    std::string otherVersion = text;
    otherVersion.replace(otherVersion.find("version=2\n"), 10, "version=1\n");
    CHECK(!sw::parse_marker(otherVersion).has_value());
    std::string noIdentity = text;
    noIdentity.replace(noIdentity.find("runtime_identity="), 17, "runtime_identitx=");
    CHECK(!sw::parse_marker(noIdentity).has_value());
    std::string noDriver = text;
    noDriver.replace(noDriver.find("driver="), 7, "drivxr=");
    CHECK(!sw::parse_marker(noDriver).has_value());
}

void test_eta() {
    sw::Eta eta;
    eta.start(0.0, 0);
    CHECK(!eta.seconds_left(2.0, 50, 500).has_value());   // too early
    CHECK(!eta.seconds_left(30.0, 5, 500).has_value());   // too few
    const auto left = eta.seconds_left(10.0, 100, 600);   // 10/s, 500 left
    CHECK(left.has_value() && *left > 49.0 && *left < 51.0);
    CHECK(!eta.seconds_left(10.0, 600, 600).has_value());  // done
    CHECK(sw::eta_text(std::nullopt).empty());
    CHECK(sw::eta_text(50.0) == "About 50 s left");
    CHECK(sw::eta_text(3.0) == "About 10 s left");
    CHECK(sw::eta_text(270.0) == "About 5 min left");
    CHECK(sw::counter_text(312, 584) == "312 / 584");
    CHECK(sw::counter_text(600, 584) == "584 / 584");
}

void test_lab_options() {
    auto o = sw::parse_lab_options("");
    CHECK(o.journal.empty() && o.prep == sw::PrepMode::Auto && !o.masterTour && o.unknown.empty());
    o = sw::parse_lab_options(
        "# lab\ncache_journal = delete\r\ncache_sync=full\ncache_check=always\nprep=force\nshader_tour=1\n");
    CHECK(o.journal == "delete" && o.sync == "full" && o.check == "always");
    CHECK(o.prep == sw::PrepMode::Force && o.masterTour && o.unknown.empty());
    CHECK(o.warmupDuty == 0);
    CHECK(sw::parse_lab_options("seed_limit=2000").seedLimit == 2000);
    CHECK(sw::parse_lab_options("seed_limit=abc").seedLimit == 0);
    CHECK(sw::parse_lab_options("shader_tour=all").harvestTour && sw::parse_lab_options("shader_tour=all").masterTour);
    CHECK(!sw::parse_lab_options("shader_tour=1").harvestTour);
    CHECK(sw::parse_lab_options("warmup_duty=50\n").warmupDuty == 50);
    CHECK(sw::parse_lab_options("warmup_duty=5\n").warmupDuty == 0);    // below 10: ignored
    CHECK(sw::parse_lab_options("warmup_duty=150\n").warmupDuty == 0);  // above 100: ignored
    CHECK(!sw::parse_lab_options("warmup_duty=half\n").unknown.empty());
    o = sw::parse_lab_options("prep=off\ncache_journal=bogus\ntypo=1\n");
    CHECK(o.prep == sw::PrepMode::Off && o.journal.empty());
    CHECK(o.unknown.find("cache_journal=bogus") != std::string::npos && o.unknown.find("typo=1") != std::string::npos);
}

}  // namespace

int main() {
    test_decide();
    test_marker();
    test_eta();
    test_lab_options();
    if (failures == 0) {
        std::cout << "shader_warmup: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
