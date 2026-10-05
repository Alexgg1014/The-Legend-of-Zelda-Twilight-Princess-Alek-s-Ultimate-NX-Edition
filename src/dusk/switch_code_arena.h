#pragma once
// aleks2 (H37): what the Switch hook backend (platforms/switch/source/funchook_os_switch.c and
// code_arena.c) tells the rest of the game: where its log lines go, and a one-line summary of the
// code arenas -- CodeMemory objects held and at most, pages, allocations, reuses, last failure.
#include <cstddef>

extern "C" {
void funchook_switch_set_logger(void (*logger)(const char* line));
void funchook_switch_describe(char* out, size_t outlen);
}
