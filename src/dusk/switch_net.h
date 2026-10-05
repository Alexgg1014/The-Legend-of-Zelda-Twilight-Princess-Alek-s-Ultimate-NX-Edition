// aleks2: see switch_net.cpp -- bringing up BSD sockets so libcurl (and with it
// the online mod browser, mod updates and the guide downloader) can reach the
// network on Switch.
#pragma once

namespace dusk::switchnet {

// False when the sockets service did not come up; everything online then fails
// with what looks like a connection error.
bool available() noexcept;

// One line in the session log saying which of the two it was.
void log_status();

}  // namespace dusk::switchnet
