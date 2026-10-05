// aleks2: libelf stand-ins for Naga's Mesa archive. The only consumer is
// nv_cubin.c (VK_NVX_binary_import, CUDA cubin loading), which the game never
// touches, and devkitPro ships no libelf portlib. Each stub reports failure
// the way libelf does, so the path fails cleanly if it were ever reached.
#include <stddef.h>

typedef struct Elf Elf;
typedef struct Elf_Scn Elf_Scn;
typedef struct Elf_Data Elf_Data;

unsigned elf_version(unsigned version) { (void)version; return 0; /* EV_NONE */ }
Elf* elf_memory(char* image, size_t size) { (void)image; (void)size; return NULL; }
int elf_kind(Elf* elf) { (void)elf; return 0; /* ELF_K_NONE */ }
int elf_end(Elf* elf) { (void)elf; return 0; }
int elf_errno(void) { return 1; }
const char* elf_errmsg(int err) { (void)err; return "libelf unavailable (aleks2 stub)"; }
int elf_getshdrstrndx(Elf* elf, size_t* dst) { (void)elf; (void)dst; return -1; }
Elf_Scn* elf_getscn(Elf* elf, size_t index) { (void)elf; (void)index; return NULL; }
Elf_Scn* elf_nextscn(Elf* elf, Elf_Scn* scn) { (void)elf; (void)scn; return NULL; }
Elf_Data* elf_getdata(Elf_Scn* scn, Elf_Data* data) { (void)scn; (void)data; return NULL; }
char* elf_strptr(Elf* elf, size_t section, size_t offset) { (void)elf; (void)section; (void)offset; return NULL; }
