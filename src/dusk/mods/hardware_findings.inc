// aleks2 (H38-c6): what the console showed about exact mod packages on this Switch build. The one source of that
// knowledge: src/dusk/mods/hardware_findings.hpp compiles it into the game (the mod browser and the update check
// read it on Switch) and platforms/switch/tools/h38_policy.py reads this same file for the H38 gate's report.
//
// Only demonstrated facts go here, for an exact package: a mod id, its mod.json version and, when known, the SHA-256
// of the .dusk that was run. A package that is not listed -- another version, other bytes, another mod -- is
// "unverified": never blocked by an entry written for something else. The static checks of the gate are not
// findings; passing them does not put a package here.
//
// h38_policy.py parses this file with a regular expression: one entry per line, exactly this form, plain strings
// (no escapes, no concatenation).
//
// DUSK_MOD_HARDWARE(id, version, sha256 of the .dusk or "", status, reason)
//   status: Tested        ran on the console and did what it is for
//           Inactive      installs on the console but stays inactive on this build
//           Incompatible  harmful on this build (crash, corruption): the browser refuses to install it
//           Unverified    known, but not shown on the console yet (the default for anything unlisted)
// DUSK_MOD_SWITCH_VARIANT(id, note)
//   H38 delivers its own switch-arm64 build of this mod. The catalog carries another build (Linux) that is not a
//   substitute: the browser never installs it or updates the Switch package with it.

// clang-format off
DUSK_MOD_HARDWARE("alexandre.cooper_hat.texture_selector", "1.1.0", "2491af27748df137e8a3ce97a8b18b9d1c5ade19894c503e0498118134056430", Tested, "H38-c5 console: active, Link's hat model rebuilt.")
DUSK_MOD_HARDWARE("dev.automata.deferred_fog", "2.0.0", "da70c3c2bf37bcc721423d5de3c0dbb740e6877820481358b9959119579a4e8b", Incompatible, "Crashes while compiling fog.wgsl on Switch: the Dawn/Tint worker thread exhausts its stack.")
DUSK_MOD_HARDWARE("com.agustinfuac.fairy_companion", "1.0.0", "06fc04d50574600f0c722c1309e3dc8c98d6d31729b32b18909f11dab1c03f01", Unverified, "Passes the static checks; not run on the console yet.")
DUSK_MOD_HARDWARE("com.kite.kh2camera", "1.0.1", "f63ea091636a3b1b5fe11bde896925d11107f4ab01b5abdda7195a01420f9765", Unverified, "Passes the static checks; not run on the console yet.")
DUSK_MOD_HARDWARE("org.dusklight.tp_classic_buttons", "1.3.1", "be891f39df113617192a54e609b866a776de50f5eab220406c864eda76b822fd", Incompatible, "Crashes this build (Data Abort in before_pane_trans): its Linux library is a Debug build that reads the game's structures with the Debug layout.")
DUSK_MOD_HARDWARE("dev.twilitrealm.randomizer", "1.0.6", "d3ae42115b62c1e240754091a433d74c3756ef03f4a7de06d748bbe4bfce39aa", Tested, "The switch-arm64 package delivered with H38: ran on the console.")
DUSK_MOD_HARDWARE("com.dusklight.twilit_essentials", "2.5.0", "6e160c870f56e6b23aa1678ed3d8da80a0cf0f953a83ef29d89eaa18099c6437", Tested, "The switch-arm64 package delivered with H38: ran on the console.")
DUSK_MOD_SWITCH_VARIANT("dev.twilitrealm.randomizer", "This build ships its own Switch Randomizer. The catalog's copy is the Linux build, which crashes the game: keep the Switch package.")
DUSK_MOD_SWITCH_VARIANT("com.dusklight.twilit_essentials", "This build ships its own Switch Twilit Essentials. The catalog's copy is the Linux build and does not replace it: keep the Switch package.")
// clang-format on
