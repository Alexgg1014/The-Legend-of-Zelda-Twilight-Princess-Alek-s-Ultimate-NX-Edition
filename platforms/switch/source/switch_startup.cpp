// How the program starts and stops on Horizon.
//
// libnx runs the program's life-cycle hooks around main(), and which hooks it
// finds are decided by symbols the program defines: __nx_applet_type chooses what
// kind of process this is, __nx_heap_size how big the newlib heap is, and
// userAppInit/userAppExit run once before and after main with the services the
// program wants open. This file defines them (it lives in the runtime library
// that is linked whole, so the linker always sees them), and begin_session() for
// the checks main() makes on entry.
#include "switch/startup.hpp"

#include "switch/fs.hpp"

#include <switch.h>

#include <cstdio>
#include <cstdlib>

namespace dusk::sw {
void vulkan_env_setup();
}

extern "C" {

// A full application, not an applet: applets get a small slice of memory.
u32 __nx_applet_type = AppletType_Application;

// 0 lets libnx give the heap everything the system leaves over.
size_t __nx_heap_size = 0;

// V1-rc12: libnx's (weak, not in its headers). 1 makes its applet teardown end an application the way AM expects
// (ISelfController::Exit, then wait for AM: _appletExitProcess); 0, under hbloader, leaves that to the loader. Set to
// 1 only when the process is to end (userAppExit), so a return to the loader stays as it was.
u32 __nx_applet_exit_mode = 0;

// V1-rc12: an application that ends with a bare svcExitProcess has, to AM, vanished: HOME shows "The software was
// closed" although nothing crashed (be6bfec on the console: clean exit.txt, no crash report, the dialog every
// time). The end AM expects is ISelfController::Exit (command 0), the request libnx's own teardown sends for an
// application before it waits to be ended. Sent here while the applet session is still open; AM then ends the
// process. If the request fails, or AM has not ended it within 10 s, the kernel does.
[[noreturn]] static void end_process_via_am(void) {
  const AppletType type = appletGetAppletType();
  if (type == AppletType_Application || type == AppletType_SystemApplication) {
    Service* self = appletGetServiceSession_SelfController();
    if (self != nullptr && serviceIsActive(self) && R_SUCCEEDED(serviceDispatch(self, 0))) {
      for (int i = 0; i < 100; ++i) {
        svcSleepThread(100'000'000LL);
      }
    }
  }
  svcExitProcess();
}

void aleks2_exit_trace(const char* line); extern unsigned long long aleks2_boot_entry_tick, aleks2_boot_romfs_tick;
static void exit_trace_atexit_last(void);

void userAppInit(void) {
  aleks2_boot_entry_tick = armGetSystemTick(); romfsInit(); aleks2_boot_romfs_tick = armGetSystemTick();  // res/, the files that ship inside the NRO; rc17: boot timing (session_log.cpp)
  dusk::sw::vulkan_env_setup(); // before nv: the driver reads its environment on init
  nvInitialize();               // the GPU service the Vulkan driver talks to
  // V1-rc10: registered before any static constructor runs, so it runs after every atexit handler and static
  // destructor (the mods' included): the exit trace's "atexit done" line.
  std::atexit(exit_trace_atexit_last);
}

// V1-rc9: after the program's atexit handlers (mods' static destructors run in their own code), the mods' images
// and the hook arenas are given back before the process returns to hbloader (aleks2_release_code_at_exit).
// V1-rc10: rc9 on the console still crashed the loader (hbl+0x4030, 0xD401, nine times) and wrote no exit.txt,
// although 8 images were registered: this release never ran, or its fopen failed. So: an exit trace written with
// the FS service directly (no stdio), one line per stage, always; an audit of the address space once everything
// is given back; and exit_mode (perf-lab.ini) -- auto ends the process instead of returning to the loader, except
// for a requested relaunch with a clean heap (the crash also happens with no external mod loaded).
void aleks2_release_code_at_exit(void);
static bool audit_heap_at_exit(void);

unsigned g_duskExitMode = 0;     // 0 auto, 1 loader, 2 process (frame_telemetry.cpp sets it from perf-lab.ini)
static bool s_endProcess = false;
static bool s_relaunchRequested = false;  // aleks2_relaunch_on_exit (below)
static LoaderReturnFn s_loaderReturn = nullptr;  // V1-rc12: the loader's exit function, before appletExit

void userAppExit(void) {
  aleks2_exit_trace("userAppExit: entered");
  aleks2_release_code_at_exit();
  nvExit();
  romfsExit();
  aleks2_exit_trace("userAppExit: nv and romfs closed");
  const bool leak = audit_heap_at_exit();
  // auto: the crash happens with no external mod at all (rc9 console), so the audit cannot be what decides: a plain
  // quit ends the process (back to HOME); only a requested relaunch (Restart now, an update) goes back to the
  // loader, and only with a clean heap.
  s_endProcess = g_duskExitMode == 2 || (g_duskExitMode == 0 && (leak || !s_relaunchRequested));
  if (s_endProcess) {
    s_loaderReturn = envGetExitFuncPtr();
    __nx_applet_exit_mode = 1;  // V1-rc12: appletExit (next in __appExit) then ends us through AM, not bare
  }
  char line[160];
  std::snprintf(line, sizeof line, "exit: %s (exit_mode %s%s%s)",
      s_endProcess ? "the process ends here, back to HOME" : "returning to the loader",
      g_duskExitMode == 1 ? "loader" : g_duskExitMode == 2 ? "process" : "auto",
      leak ? ", heap not clean" : ", heap clean", s_relaunchRequested ? ", relaunch requested" : "");
  aleks2_exit_trace(line);
}

// libnx's own is weak: the same steps, and the process ends after the services are closed when userAppExit
// decided so (a requested relaunch is dropped then: the loader would not survive it).
void __appExit(void);
void NX_NORETURN __nx_exit(Result rc, LoaderReturnFn retaddr);

void NX_NORETURN __libnx_exit(int rc) {
  (void)rc;
  __appExit();
  // V1-rc12: with s_endProcess, __nx_applet_exit_mode 1 made appletExit point the exit function at libnx's
  // _appletExitProcess (ISelfController::Exit, then AM ends the process); without it, back to hbloader. If the
  // applet teardown did not run (another reference held it), it is still the loader's: end the process as before.
  if (s_endProcess && envGetExitFuncPtr() == s_loaderReturn) {
    svcExitProcess();
  }
  __nx_exit(0, envGetExitFuncPtr());
}

int aleks2_release_mod_images(int* released, uint32_t* lastRc);        // switch_elf_memory.cpp
int aleks2_release_hook_arenas(uint32_t* released, uint32_t* last_rc);  // funchook_os_switch.c

// When anything cannot be given back, returning to hbloader would crash it: the process ends here instead (back
// to HOME, as the exit guard does for a live thread).
void aleks2_release_code_at_exit(void) {
  int images = 0;
  uint32_t arenas = 0, rc = 0;
  const int failures = aleks2_release_mod_images(&images, &rc) + aleks2_release_hook_arenas(&arenas, &rc);
  char line[160];
  std::snprintf(line, sizeof line, "released %d mod image(s) and %u hook arena(s); %d failed%s", images,
      static_cast<unsigned>(arenas), failures, failures != 0 ? " (the process ends here)" : "");
  aleks2_exit_trace(line);
  if (failures != 0) {
    std::snprintf(line, sizeof line, "last failure: rc 0x%x", static_cast<unsigned>(rc));
    aleks2_exit_trace(line);
    end_process_via_am();
  }
}

// One line appended to sdmc:/switch/dusklight/cache/logs/exit.txt (next to the session logs) through the FS
// service: works after stdio is torn down, until __appExit unmounts the card. The first line of a run says when it
// started. V1-rc11: rc8-rc10 wrote to dusklight/logs/, a folder that does not exist on the card, so nothing was
// ever written (rc10's console run had no exit trace at all for that reason only).
void aleks2_exit_trace(const char* line) {
  FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
  if (sd == nullptr || line == nullptr) {
    return;
  }
  // libnx sends FS_MAX_PATH bytes from the pointer it is given: full-size buffers, as dev_mode.cpp does.
  static char kPath[FS_MAX_PATH] = "/switch/dusklight/cache/logs/exit.txt";
  static bool s_dirs = false;
  if (!s_dirs) {
    s_dirs = true;
    char dir[FS_MAX_PATH] = "/switch/dusklight/cache";
    fsFsCreateDirectory(sd, dir);  // fail when they exist
    std::snprintf(dir, sizeof dir, "/switch/dusklight/cache/logs");
    fsFsCreateDirectory(sd, dir);
  }
  fsFsCreateFile(sd, kPath, 0, 0);  // fails when it exists
  FsFile f;
  if (R_FAILED(fsFsOpenFile(sd, kPath, FsOpenMode_Write | FsOpenMode_Append, &f))) {
    return;
  }
  s64 size = 0;
  fsFileGetSize(&f, &size);
  static bool s_first = true;
  char buf[256];
  int n = 0;
  if (s_first) {
    s_first = false;
    const u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ull;
    n = std::snprintf(buf, sizeof buf, "--- exit (process up %llu.%03llu s)\n", ms / 1000ull, ms % 1000ull);
    if (n > 0) {
      fsFileWrite(&f, size, buf, static_cast<u64>(n), FsWriteOption_Flush);
      size += n;
    }
  }
  n = std::snprintf(buf, sizeof buf, "%s\n", line);
  if (n > 0) {
    fsFileWrite(&f, size, buf, static_cast<u64>(n) < sizeof buf ? static_cast<u64>(n) : sizeof buf - 1,
        FsWriteOption_Flush);
  }
  fsFileClose(&f);
}

static void exit_trace_atexit_last(void) {
  aleks2_exit_trace("exit(): atexit handlers and static destructors done");
}

// Every region of the address space that is not plain: our heap (the loader's heap override) should be all
// Heap/RW with no attribute once the mods, arenas, threads and GPU memory are given back; anything else in it
// (locked as a code alias's or a thread stack's source, borrowed by IPC, device-mapped, transfer memory) is what a
// loader that reuses the heap trips on. Outside the heap, the mappings still there (code aliases, CodeMemory,
// stacks, transfer and shared memory) are listed for the record. Returns whether the heap is not clean.
static bool audit_heap_at_exit(void) {
  u64 heapStart = 0, heapEnd = 0;
  if (envHasHeapOverride()) {
    heapStart = reinterpret_cast<u64>(envGetHeapOverrideAddr());
    heapEnd = heapStart + envGetHeapOverrideSize();
  } else {
    u64 size = 0;
    svcGetInfo(&heapStart, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&size, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
    heapEnd = heapStart + size;
  }
  unsigned bad = 0, listed = 0, other = 0;
  u64 badBytes = 0;
  char line[200];
  u64 addr = 0;
  for (int guard = 0; guard < 4096; ++guard) {
    MemoryInfo mi{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&mi, &page, addr)) || mi.size == 0) {
      break;
    }
    const u32 type = mi.type & MemState_Type;
    const bool inHeap = mi.addr < heapEnd && mi.addr + mi.size > heapStart;
    const bool plainHeap = type == MemType_Heap && mi.attr == 0 && mi.perm == Perm_Rw;
    const bool boring = type == MemType_Unmapped || type == MemType_Reserved || type == MemType_Io ||
                        type == MemType_ThreadLocal || type == MemType_KernelStack ||
                        type == MemType_CodeStatic || type == MemType_CodeMutable ||
                        type == MemType_ModuleCodeStatic || type == MemType_ModuleCodeMutable;
    if (inHeap && !plainHeap) {
      ++bad;
      badBytes += mi.size;
    }
    if ((inHeap && !plainHeap) || (!inHeap && !boring && type != MemType_Heap)) {
      if (listed < 40) {
        ++listed;
        std::snprintf(line, sizeof line, "  %s 0x%010llx +0x%llx type 0x%x attr 0x%x perm %u ipc %u dev %u",
            inHeap ? "HEAP" : "map ", static_cast<unsigned long long>(mi.addr),
            static_cast<unsigned long long>(mi.size), mi.type, mi.attr, mi.perm, mi.ipc_refcount,
            mi.device_refcount);
        aleks2_exit_trace(line);
      } else {
        ++other;
      }
    }
    const u64 next = mi.addr + mi.size;
    if (next <= addr) {
      break;
    }
    addr = next;
  }
  std::snprintf(line, sizeof line, "audit: heap 0x%llx-0x%llx, %u region(s) not plain heap (%llu KiB)%s",
      static_cast<unsigned long long>(heapStart), static_cast<unsigned long long>(heapEnd), bad,
      static_cast<unsigned long long>(badBytes / 1024), other != 0 ? ", more not listed" : "");
  aleks2_exit_trace(line);
  return bad != 0;
}

}  // extern "C"

namespace dusk::sw {

namespace {

// Shown on the text console when started as an applet. The player has no other
// way to find out why nothing came up.
void explain_title_takeover() {
  consoleInit(nullptr);
  std::puts("Dusklight has to be started as a title takeover.");
  std::puts("");
  std::puts("Start a game while holding R (or use a forwarder), then pick");
  std::puts("Dusklight from the homebrew menu.");
  std::puts("");
  std::puts("Press PLUS to quit.");
  consoleUpdate(nullptr);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
      break;
    }
    consoleUpdate(nullptr);
  }
  consoleExit(nullptr);
}

}  // namespace

void debug_print(std::string_view line) {
  if (envIsSyscallHinted(0x27)) {
    svcOutputDebugString(line.data(), line.size());
  }
}

bool begin_session() {
  if (appletGetAppletType() != AppletType_Application) {
    explain_title_takeover();
    return false;
  }

  // The main thread stays on core 0; the game's own worker threads are placed on
  // the other cores as they start.
  svcSetThreadCoreMask(threadGetCurHandle(), 0, 1u << 0);

  ensure_data_root();
  return true;
}

}  // namespace dusk::sw

// aleks2 (V1-rc4): start this NRO again when the program exits (hbloader honours a next-load request): the
// "Restart now" button of settings that take effect at the next start. False when the loader that started
// us does not take a next-load request; the caller then asks the player to restart by hand.
extern "C" int __system_argc;
extern "C" char** __system_argv;
extern "C" int aleks2_live_mod_threads(void);        // elf_runtime_policy.cpp: a mod's pthread_create import
extern "C" int aleks2_live_threads_from_mods(void);  // switch_threads.cpp: any pthread_create from a mod's code
extern "C" uint32_t aleks2_sockets_created(void);    // switch_sockets.cpp
static int live_mod_threads() { return aleks2_live_mod_threads() + aleks2_live_threads_from_mods(); }

// V1-rc8: also false while a mod's thread runs: the exit then ends the process (aleks2_exit_guard), which no
// relaunch survives, so the caller asks for a restart by hand instead of closing to HOME without a word.
// V1-rc11: also false once this process has used the network. A relaunch runs in the same process (hbloader does
// not end it), and the ssl, nifm and csrng sessions libcurl opened (it frees them only in curl_global_cleanup) and
// the sockets the ssl sysmodule took over outlive the NRO: on a tester's console (rc9, Atmosphère 1.11.2, 22.5.0)
// the next start in that process made bsdsocket abort (2162-0002, the whole system down), twice. Ending the process
// lets the kernel close all of it.
// V1-rc12: never, for now. rc11's exit audit found eight of our own threads still running at exit (three 8 MiB
// stacks, five 128 KiB ones: none is joined when the game quits), so a relaunch -- which has to give the mods' code
// and the hook arena back first -- unmaps code those threads still call through: that is the loader crash.
// Restart now and the updater ask the player to quit and open again (aleks2_relaunch_disabled_reason).
extern "C" bool aleks2_relaunch_on_exit(void) {
  if (true) {
    return false;
  }
  if (live_mod_threads() > 0) {
    return false;
  }
  if (aleks2_sockets_created() > 0) {
    return false;
  }
  if (!envHasNextLoad() || __system_argc < 1 || __system_argv == nullptr || __system_argv[0] == nullptr) {
    return false;
  }
  s_relaunchRequested = R_SUCCEEDED(envSetNextLoad(__system_argv[0], __system_argv[0]));
  return s_relaunchRequested;
}

// V1-rc12: called by DuskMain once game_main has returned (update applied, mods unloaded, config saved, logs
// closed). A plain quit ends the process here, before exit() runs the static destructors and userAppExit gives the
// mods' images and the hook arena back while the game's own threads still run (rc11 console: released, then a
// thread called a hooked function during the audit's SD writes and the loader stopped with 2001-0106). The kernel
// takes back everything -- threads, mappings, sessions, transfer memory -- at once. exit_mode=loader skips this
// (the rc11 path: release, audit, then return or end), for testing only.
extern "C" void aleks2_end_process_on_quit(void) {
  if (g_duskExitMode == 1 || s_relaunchRequested) {
    aleks2_exit_trace("exit: going on to the loader path (exit_mode loader or a relaunch)");
    return;
  }
  std::fflush(nullptr);  // whatever stdio still buffers: the static destructors that would close it do not run
  aleks2_exit_trace("exit: the process ends here, back to HOME (before any code is unmapped; AM asked to end it)");
  end_process_via_am();
}

// aleks2 (V1-rc8): the last thing before returning to hbloader. A thread still running then runs in an NRO hbloader
// is unmapping, and hbloader stops with a User Break (2001-0106 on two consoles with rc4/rc7, after a clean "all mods
// unloaded"). The live thread's stack (0x21000) was that of the std::thread Twilit Essentials 2.8.4 starts in its
// mod_initialize: a mod is unloaded, its threads are not. Horizon cannot stop another thread, so when a mod's thread
// is still running the process ends here instead: back to HOME rather than the Homebrew Menu, without a crash (and
// without a requested relaunch, which hbloader would not survive either).
extern "C" void aleks2_exit_guard(void) {
  const int live = live_mod_threads();
  if (live <= 0) {
    return;
  }
  if (std::FILE* f = std::fopen("sdmc:/switch/dusklight/cache/logs/exit.txt", "a")) {
    std::fprintf(f, "%d mod thread(s) still running at exit (%d via the import, %d via the C++ library); the "
                    "process ends here instead of returning to hbloader\n",
        live, aleks2_live_mod_threads(), aleks2_live_threads_from_mods());
    std::fclose(f);
  }
  end_process_via_am();
}

// aleks2 (rc17): the boot timing's first points (boot_timing.hpp): the process entry and the romfs mount, in
// system ticks. Plain zero-initialised globals: userAppInit runs before any static constructor.
extern "C" {
unsigned long long aleks2_boot_entry_tick = 0;
unsigned long long aleks2_boot_romfs_tick = 0;
}
