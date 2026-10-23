#pragma once
// aleks2 (DM2-d): dl_iterate_phdr for Linux-built modules, over the images this process really has: the
// game (described from its linker-script symbols on Switch) and every module elf_loader mapped.
//
// glibc's contract (elf/dl-iteratephdr.c, include/link.h): the callback gets a struct dl_phdr_info -- 64 bytes
// on LP64: dlpi_addr (load bias), dlpi_name ("" for the executable), dlpi_phdr/dlpi_phnum (the program
// headers, link-time vaddrs), dlpi_adds/dlpi_subs (objects ever added/removed, which libgcc's FDE cache
// keys on), dlpi_tls_modid (the TLS module id, 0 if none) and dlpi_tls_data (this thread's block of it, or
// NULL if not allocated yet) -- with size = sizeof that struct, executable first, until it returns non-zero.
//
// Users seen: Twilit Essentials 2.8.4 finds its own executable PT_LOAD and fingerprints it; a module linked
// with a static libgcc_eh finds its PT_GNU_EH_FRAME through it to unwind. Modules are never unloaded, so
// dlpi_subs stays 0. dlpi_phdr points to a copy of the file's table kept here (glibc's points into the
// mapping; callers only read it).
#include "elf_runtime_policy.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dusk::mods::elf::dl {

struct Object {
    std::string name;
    uint64_t bias = 0;                // runtime address minus link-time vaddr
    std::vector<uint8_t> phdrTable;   // Elf64_Phdr[phnum], as in the file
    uint16_t phnum = 0;
    uint64_t tlsModule = 0;           // elf::tls module id, 0 if none
};

// The executable, listed first. Replaces a previous one.
void set_main(Object object);
// A module that mapped. Kept for the life of the process.
void add(Object object);
// How this thread's block of a TLS module is found (elf::tls::current_block); unset, dlpi_tls_data is NULL.
void set_tls_block_lookup(void* (*lookup)(uint64_t module));

// The shim, Linux-built modules only.
std::vector<Shim> shims();

// For tests and logs.
size_t object_count();

}  // namespace dusk::mods::elf::dl

extern "C" int aleks2_dl_iterate_phdr(int (*callback)(void* info, size_t size, void* data), void* data);

// glibc 2.35's _dl_find_object, which libgcc 12+ (unwind-dw2-fde-dispatch.c) uses instead of dl_iterate_phdr
// to find the FDEs for a pc when it was built against a glibc that has it: a module with a static libgcc_eh
// imports it. Fills glibc's struct dl_find_object (dlfo_flags, dlfo_map_start, dlfo_map_end, dlfo_link_map
// = NULL, dlfo_eh_frame = the object's PT_GNU_EH_FRAME); 0 if an object holds pc, -1 otherwise.
extern "C" int aleks2_dl_find_object(void* pc, void* result);
