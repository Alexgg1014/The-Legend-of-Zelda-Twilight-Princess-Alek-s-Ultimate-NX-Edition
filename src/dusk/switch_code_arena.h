#pragma once
// aleks2 (H37): what the Switch hook backend (platforms/switch/source/funchook_os_switch.c and
// code_arena.c) tells the rest of the game: where its log lines go, and a one-line summary of the
// code arenas -- CodeMemory objects held and at most, hook slots, allocations, reuses, last failure.
#include <cstddef>
#include <cstdint>

extern "C" {
void funchook_switch_set_logger(void (*logger)(const char* line));
void funchook_switch_describe(char* out, size_t outlen);
// DM2-a: hook slots in use, free in the arenas held, and every slot the arenas could hold
// (code_arena_usage). One slot per hooked function.
void funchook_switch_usage(uint32_t* used, uint32_t* free_now, uint32_t* capacity);
}
