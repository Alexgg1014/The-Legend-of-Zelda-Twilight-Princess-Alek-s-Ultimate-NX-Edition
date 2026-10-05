#pragma once

namespace dusk::sw {

// Creates the folders the program keeps its files in on the SD card:
// sdmc:/switch/dusklight and its cache folder. Each level is made on its own, and
// one that already exists is not an error.
void ensure_data_root();

}  // namespace dusk::sw
