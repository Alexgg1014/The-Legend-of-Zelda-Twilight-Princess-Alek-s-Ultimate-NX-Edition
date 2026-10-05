// borealis::net for Horizon: the socket layer is not implemented.
//
// It could be: Horizon offers BSD sockets once socketInitializeDefault has run
// (src/dusk/switch_net.cpp does that), and HTTP already goes over them through
// libcurl. What is missing is the layer on top, a socket table with a poll loop.
// It matters for the co-op mod, which imports this service and will not load
// without it. Until it exists every call fails as it would with the cable out: a
// result the caller already handles, never a crash and never a silent success.
#include <borealis/net.hpp>

#include <string>

namespace borealis::net {

struct Context::Impl {};

Context::Context(ContextOptions) : m_impl{std::make_unique<Impl>()} {}
Context::~Context() = default;

// Socket id 0 is borealis' "no socket".
SocketId Context::connect(std::string_view, StreamOptions, void*) { return 0; }
SocketId Context::resolve(std::string_view, void*) { return 0; }

Context::BindResult Context::listen(std::string_view, ListenOptions, void*) {
  return {0, {}, Error::Network, "sockets are not implemented on this console"};
}
Context::BindResult Context::open_datagram(std::string_view, DatagramOptions, void*) {
  return {0, {}, Error::Network, "sockets are not implemented on this console"};
}

SendResult Context::send(SocketId, std::span<const std::byte>) { return SendResult::NotOpen; }
SendResult Context::send_to(SocketId, std::string_view, std::span<const std::byte>) { return SendResult::NotOpen; }
void Context::set_user_data(SocketId, void*) {}
std::optional<Stats> Context::stats(SocketId) const { return std::nullopt; }
void Context::close(SocketId) {}
bool Context::poll(Event&) { return false; }

bool available() noexcept { return false; }

// Parsing would need the resolver to tell hosts from addresses, so it declines.
std::optional<ParsedEndpoint> parse_endpoint(std::string_view) { return std::nullopt; }

// "scheme://host:port", the host in brackets if it is an IPv6 address.
std::string format_endpoint(const ParsedEndpoint& endpoint) {
  const std::string host = endpoint.ipv6 ? "[" + endpoint.host + "]" : endpoint.host;
  return endpoint.scheme + "://" + host + ":" + std::to_string(endpoint.port);
}

}  // namespace borealis::net
