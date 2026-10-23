#include "winhttp_transport.hpp"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

namespace crosssave::smoke {
namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

}  // namespace

WinHttpTransport::WinHttpTransport() {
    m_session = WinHttpOpen(L"Dusklight-CloudSave-Smoke/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
}

WinHttpTransport::~WinHttpTransport() {
    if (m_session != nullptr) {
        WinHttpCloseHandle(m_session);
    }
}

void WinHttpTransport::send(firebase::HttpRequest request, std::function<void(firebase::HttpResponse)> done) {
    ++requests;
    firebase::HttpResponse response;
    const auto fail = [&](const char* what) {
        response.transportOk = false;
        response.error = what + std::string{" (WinHTTP error "} + std::to_string(GetLastError()) + ")";
        done(std::move(response));
    };
    const std::wstring url = widen(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    wchar_t extra[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 4096;
    if (m_session == nullptr || !WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        fail("bad URL");
        return;
    }
    HINTERNET connection = WinHttpConnect(m_session, host, parts.nPort, 0);
    if (connection == nullptr) {
        fail("connect");
        return;
    }
    const std::wstring target = std::wstring{path} + extra;
    HINTERNET handle = WinHttpOpenRequest(connection, widen(request.method).c_str(), target.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (handle == nullptr) {
        WinHttpCloseHandle(connection);
        fail("open request");
        return;
    }
    std::wstring headers;
    for (const auto& [name, value] : request.headers) {
        headers += widen(name) + L": " + widen(value) + L"\r\n";
    }
    const BOOL sent = WinHttpSendRequest(handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(-1L), request.body.empty() ? WINHTTP_NO_REQUEST_DATA : request.body.data(),
        static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(handle, nullptr)) {
        WinHttpCloseHandle(handle);
        WinHttpCloseHandle(connection);
        fail("send/receive");
        return;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &size, WINHTTP_NO_HEADER_INDEX);
    std::string body;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handle, &available) || available == 0) {
            break;
        }
        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(handle, chunk.data(), available, &read) || read == 0) {
            break;
        }
        body.append(chunk.data(), read);
        if (body.size() > request.maxResponseBytes) {
            WinHttpCloseHandle(handle);
            WinHttpCloseHandle(connection);
            fail("response too large");
            return;
        }
    }
    WinHttpCloseHandle(handle);
    WinHttpCloseHandle(connection);
    response.transportOk = true;
    response.status = static_cast<int>(status);
    response.body = std::move(body);
    done(std::move(response));
}

}  // namespace crosssave::smoke
