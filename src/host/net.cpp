#include "net.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#include "engine.hpp"
#include "plugins.hpp"

namespace net {
namespace {

// The functions used, looked up once from the system DLLs.
struct WinHttp {
    decltype(&WinHttpOpen) open = nullptr;
    decltype(&WinHttpCrackUrl) crackUrl = nullptr;
    decltype(&WinHttpConnect) connect = nullptr;
    decltype(&WinHttpOpenRequest) openRequest = nullptr;
    decltype(&WinHttpSendRequest) sendRequest = nullptr;
    decltype(&WinHttpReceiveResponse) receiveResponse = nullptr;
    decltype(&WinHttpQueryHeaders) queryHeaders = nullptr;
    decltype(&WinHttpReadData) readData = nullptr;
    decltype(&WinHttpCloseHandle) closeHandle = nullptr;
    decltype(&WinHttpSetTimeouts) setTimeouts = nullptr;
    bool ok = false;
};

template <class F>
void Resolve(HMODULE dll, const char* name, F& fn) {
    fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}

const WinHttp& Http() {
    static WinHttp h;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE dll = LoadLibraryW(L"winhttp.dll");
        if (!dll) return;
        Resolve(dll, "WinHttpOpen", h.open);
        Resolve(dll, "WinHttpCrackUrl", h.crackUrl);
        Resolve(dll, "WinHttpConnect", h.connect);
        Resolve(dll, "WinHttpOpenRequest", h.openRequest);
        Resolve(dll, "WinHttpSendRequest", h.sendRequest);
        Resolve(dll, "WinHttpReceiveResponse", h.receiveResponse);
        Resolve(dll, "WinHttpQueryHeaders", h.queryHeaders);
        Resolve(dll, "WinHttpReadData", h.readData);
        Resolve(dll, "WinHttpCloseHandle", h.closeHandle);
        Resolve(dll, "WinHttpSetTimeouts", h.setTimeouts);
        h.ok = h.open && h.crackUrl && h.connect && h.openRequest && h.sendRequest && h.receiveResponse && h.queryHeaders &&
               h.readData && h.closeHandle && h.setTimeouts;
    });
    return h;
}

// Closes a WinHTTP handle when it goes out of scope.
struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) Http().closeHandle(h);
    }
};

bool FetchFile(const std::string& url, std::string& body, std::string& error) {
    // file:///C:/dir/file -> C:\dir\file
    std::string path = url.substr(8);
    for (char& c : path)
        if (c == '/') c = '\\';
    std::ifstream f(eng::Widen(path).c_str(), std::ios::binary);
    if (!f) {
        error = "cannot read " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    body = ss.str();
    if (body.size() > kMaxDownload) {
        error = "file too large";
        return false;
    }
    return true;
}

bool FetchHttps(const std::string& url, std::string& body, std::string& error) {
    const WinHttp& http = Http();
    if (!http.ok) {
        error = "WinHTTP unavailable";
        return false;
    }
    std::wstring wide = eng::Widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!http.crackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "not an https URL";
        return false;
    }
    const std::wstring agent = eng::Widen(std::string("BallestPluginHost/") + plugins::kHostVersion);
    Handle session{http.open(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) {
        error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    http.setTimeouts(session.h, 10000, 10000, 15000, 30000);
    Handle connection{http.connect(session.h, host, parts.nPort, 0)};
    Handle request{connection.h ? http.openRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  WINHTTP_FLAG_SECURE)
                                : nullptr};
    if (!request.h || !http.sendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !http.receiveResponse(request.h, nullptr)) {
        error = "request failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, size = sizeof status;
    http.queryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                      WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    body.clear();
    char buf[16384];
    for (DWORD read = 0; http.readData(request.h, buf, sizeof buf, &read) && read > 0;) {
        body.append(buf, read);
        if (body.size() > kMaxDownload) {
            error = "download too large";
            return false;
        }
    }
    return true;
}

}  // namespace

bool Fetch(const std::string& url, std::string& body, std::string& error) {
    if (url.rfind("file:///", 0) == 0) return FetchFile(url, body, error);
    if (url.rfind("https://", 0) == 0) return FetchHttps(url, body, error);
    error = "unsupported URL: " + url;
    return false;
}

std::string Sha256(const std::string& data) {
    using OpenFn = decltype(&BCryptOpenAlgorithmProvider);
    // BCryptHash (Windows 10+) is only declared for newer SDK targets, so its type is written out.
    using HashFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, PUCHAR, ULONG, PUCHAR, ULONG);
    using CloseFn = decltype(&BCryptCloseAlgorithmProvider);
    static OpenFn open = nullptr;
    static HashFn hash = nullptr;
    static CloseFn close = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        if (HMODULE dll = LoadLibraryW(L"bcrypt.dll")) {
            Resolve(dll, "BCryptOpenAlgorithmProvider", open);
            Resolve(dll, "BCryptHash", hash);
            Resolve(dll, "BCryptCloseAlgorithmProvider", close);
        }
    });
    if (!open || !hash || !close) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (open(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    unsigned char digest[32];
    const long result = hash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()),
                             digest, sizeof digest);
    close(alg, 0);
    if (result != 0) return "";
    std::string hex;
    char b[3];
    for (unsigned char c : digest) {
        std::snprintf(b, sizeof b, "%02x", c);
        hex += b;
    }
    return hex;
}

}  // namespace net
