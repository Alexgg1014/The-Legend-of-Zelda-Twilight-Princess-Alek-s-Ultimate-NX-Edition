// borealis::ws for Horizon: WebSocket connections are not implemented.
//
// borealis' own WebSocket client sits on libcurl's WebSocket API, which needs
// libcurl 8.11, and devkitPro's portlib is 7.69. HTTP works (cmake/CurlHttp.cmake);
// this does not. Every connection is therefore created already closed, a state
// the callers in src/dusk/mods/svc/websocket.cpp handle, and available() lets a
// mod find out before it builds anything on top.
#include <borealis/ws.hpp>

namespace borealis::ws {
namespace detail {

// A connection that exists only as an object: there is nothing to keep.
class ConnectionState {};

class ConnectionAccess {
public:
  static Connection closed() { return Connection{std::shared_ptr<ConnectionState>{}}; }
};

}  // namespace detail

Connection::Connection(std::shared_ptr<detail::ConnectionState> state) : m_state{std::move(state)} {}
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;
Connection::~Connection() = default;

void Connection::reset() noexcept { m_state.reset(); }
Connection::operator bool() const noexcept { return m_state != nullptr; }
Connection::State Connection::state() const noexcept { return State::Closed; }
bool Connection::poll(Event&) { return false; }
SendResult Connection::send(MessageKind, std::string_view) { return SendResult::NotOpen; }
CloseResult Connection::close(uint16_t, std::string_view) { return CloseResult::NotOpen; }

bool available() noexcept { return false; }
Connection connect(Options) { return detail::ConnectionAccess::closed(); }

}  // namespace borealis::ws
