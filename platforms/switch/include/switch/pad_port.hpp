#pragma once

namespace dusk::sw {

// Makes sure a controller is on GameCube port 1 whenever one is connected. Cheap
// enough to call every frame, and meant to be: it does nothing unless port 1 is
// empty.
void bind_pad_to_port_one();

}  // namespace dusk::sw
