// The program's folders on the SD card. Saves, settings, mods, guides and the
// shader cache all live under sdmc:/switch/dusklight, which is where homebrew is
// expected to keep its data; what ships with the program is in the NRO's romfs and
// is read-only. The folders are made at startup rather than on first use, so a card
// that is full or freshly formatted fails there, where it is easy to understand,
// and not as a save that quietly never appears.
#include "switch/fs.hpp"

#include <sys/stat.h>

namespace dusk::sw {

void ensure_data_root() {
  // mkdir cannot make a path in one go here, and its failure for an existing
  // folder is the normal case, so the results are not checked: a real failure
  // shows up when something tries to write.
  static constexpr const char* kFolders[] = {
    "sdmc:/switch",
    "sdmc:/switch/dusklight",
    "sdmc:/switch/dusklight/cache",
  };
  for (const char* folder : kFolders) {
    mkdir(folder, 0777);
  }
}

}  // namespace dusk::sw
