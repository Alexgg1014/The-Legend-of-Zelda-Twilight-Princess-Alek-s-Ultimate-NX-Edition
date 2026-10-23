#pragma once
// aleks2 (DM3): the console self-test of the sockets layer for Linux mods. See switch_net_selftest.cpp.
// Only if <mods directory>/../net-selftest/net_lab.so exists; runs at loader start, before any mod.
#include <filesystem>

namespace dusk::switchelf {

void run_net_selftest(const std::filesystem::path& modsDirectory);

}  // namespace dusk::switchelf
