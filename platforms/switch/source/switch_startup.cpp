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

namespace dusk::sw {
void vulkan_env_setup();
}

extern "C" {

// A full application, not an applet: applets get a small slice of memory.
u32 __nx_applet_type = AppletType_Application;

// 0 lets libnx give the heap everything the system leaves over.
size_t __nx_heap_size = 0;

void userAppInit(void) {
  romfsInit();                  // res/, the files that ship inside the NRO
  dusk::sw::vulkan_env_setup(); // before nv: the driver reads its environment on init
  nvInitialize();               // the GPU service the Vulkan driver talks to
}

void userAppExit(void) {
  nvExit();
  romfsExit();
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
