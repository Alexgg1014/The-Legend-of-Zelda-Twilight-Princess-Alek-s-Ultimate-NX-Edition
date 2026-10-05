// Putting a controller on port 1.
//
// Aurora attaches a gamepad to a GameCube port when SDL reports that one was
// plugged in. On the console the pad is already there when the process starts, so
// that report has come and gone, and port 1 stays empty while a working controller
// sits in the pad list unused. A controller that disconnects and returns is in the
// same position. So nothing here waits for an event: whenever port 1 is empty, the
// first pad that is on no port at all is put there.
#include "switch/pad_port.hpp"

#include <dolphin/pad.h>

namespace dusk::sw {

namespace {

bool on_some_port(u32 pad) {
  for (u32 port = 0; port < PAD_CHANMAX; ++port) {
    if (PADGetIndexForPort(port) == static_cast<s32>(pad)) {
      return true;
    }
  }
  return false;
}

}  // namespace

void bind_pad_to_port_one() {
  if (PADGetIndexForPort(PAD_CHAN0) >= 0) {
    return;
  }
  const u32 pads = PADCount();
  for (u32 pad = 0; pad < pads; ++pad) {
    if (!on_some_port(pad)) {
      PADSetPortForIndex(pad, PAD_CHAN0);
      return;
    }
  }
}

}  // namespace dusk::sw
