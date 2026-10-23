#pragma once
// aleks2 (V1-PGO): the profile run of a profile-guided build (ALEKS2_PGO=GENERATE in CMakeLists.txt). The game, JSystem
// and Aurora are compiled with -fprofile-generate -fprofile-info-section, so no libgcov constructor writes .gcda files at
// exit (their compile-time paths do not exist on the console). Instead this writes every unit's counters, as the gcda
// stream __gcov_info_to_gcda produces, into ONE file per run: sdmc:/switch/dusklight/pgo/run-<date>-<time>.dkpgo.
// It is rewritten (a temporary file, then a rename) every 2 minutes, when the game loses focus (HOME) and at quit, so a
// run cut short still leaves its last snapshot. Each snapshot holds the counters since boot: only the latest one of a run
// counts. platforms/switch/tools/pgo_stream.py turns the files into .gcda trees; gcov-tool merge adds the runs.
// The workflow follows the idea of Chansey's sa2-nx PGO notes (our own code; THIRD_PARTY_NOTICES).
// In any other build every function here does nothing.

namespace dusk::pgo_dump {

// Early in main, right after logging starts: the writer thread is created while threads are still available (the
// first frame was too late in the emulator). Without the thread, tick() takes the snapshots itself.
void start();
// Once per presented frame (main loop): starts the writer thread the first time, and asks for a snapshot when the game
// goes out of focus.
void tick();
// Quit: the writer thread stopped and a last snapshot written from this thread.
void shutdown();

}  // namespace dusk::pgo_dump
