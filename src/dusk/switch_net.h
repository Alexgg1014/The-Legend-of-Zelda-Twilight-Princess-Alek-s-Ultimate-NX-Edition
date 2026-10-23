// aleks2: see switch_net.cpp -- bringing up BSD sockets so libcurl (and with it
// the online mod browser, mod updates and the guide downloader) can reach the
// network on Switch. DM3: also what mods' networking needs from the console
// (connection status, the IPv4 configuration, resume from sleep, the LAN option).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace dusk::switchnet {

// False when the sockets service did not come up; everything online then fails
// with what looks like a connection error.
bool available() noexcept;

// One line in the session log saying which of the two it was (DM3: with the
// session count, the transfer memory and what it cost).
void log_status();

// DM3: nifm's view. Connected = the console reports a working internet connection.
enum class Link { Unknown, Offline, Connecting, Connected };
Link link();
const char* link_name(Link link);

// The current IPv4 configuration (network byte order); false without one.
struct IpConfig {
    uint32_t address = 0, netmask = 0, gateway = 0, dns1 = 0, dns2 = 0;
};
bool ip_config(IpConfig& out);

// "HOS 18.1.0 (Atmosphere)" and friends, for uname.
std::string system_release();

// DM3: called on the main thread when the console resumes from sleep. Sockets do not survive sleep;
// listeners treat it as a disconnect. Returns an id for remove_resume_listener.
int add_resume_listener(std::function<void()> listener);
void remove_resume_listener(int id);
// How many resumes this session has seen.
uint32_t resume_count();

// DM3 LAN option (Settings, off by default): a mod's TCP server on 127.0.0.1 listens on the local network.
bool lan_listen_allowed();
void set_lan_listen_allowed(bool allowed);

}  // namespace dusk::switchnet
