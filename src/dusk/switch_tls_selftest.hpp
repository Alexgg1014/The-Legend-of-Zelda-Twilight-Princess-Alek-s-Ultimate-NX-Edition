#pragma once
// aleks2 (TLS-lab): the console self-test of module TLS (phase A of the TLS candidate's test).
//
// Runs only if <mods directory>/../tls-selftest/tls_selftest.so exists (the tester copies it there for
// phase A and removes it for phase B), once, when the native loader starts and before any mod loads.
// Writes tls-selftest/result.txt and the game log; the last line is "TLS SELFTEST RESULT: PASS|FAIL|SKIPPED".
// The module it maps stays mapped (like any mod: there is no unloading), so it takes one code mapping
// and one libnx TLS slot for the rest of that run.
#include <filesystem>

namespace dusk::switchelf {

void run_tls_selftest(const std::filesystem::path& modsDirectory);

}  // namespace dusk::switchelf
