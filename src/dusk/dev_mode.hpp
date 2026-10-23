#pragma once
// aleks2 (V1-rc5): developer mode. On when sdmc:/switch/dusklight/dev.txt exists at start (its content does not
// matter). It shows the diagnostics of Settings > Aleks (benchmarks, shader tour, DRS stress, JIT self-test, A/B
// toggles, the native Loader Mode), reads the *-lab.ini files and runs the on-card self-tests (tls-selftest,
// net-selftest, loader-only.txt). Without it a player sees none of that and native mods always run (Execute).
// docs/investigaciones/v1-limpieza-release.md lists every item.
namespace dusk {
#ifdef __SWITCH__
bool dev_mode();
#else
inline bool dev_mode() { return true; }  // PC builds keep every option where it was
#endif
}  // namespace dusk
