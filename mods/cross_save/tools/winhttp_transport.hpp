#pragma once

// A synchronous HttpTransport over WinHTTP, for the Windows smoke test against the real Firebase.
// The callback runs before send() returns.

#include "../firebase/firebase_backend.hpp"

namespace crosssave::smoke {

class WinHttpTransport : public firebase::HttpTransport {
public:
    WinHttpTransport();
    ~WinHttpTransport() override;
    void send(firebase::HttpRequest request, std::function<void(firebase::HttpResponse)> done) override;
    int requests = 0;

private:
    void* m_session = nullptr;
};

}  // namespace crosssave::smoke
