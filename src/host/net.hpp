// Downloads and hashes, with what Windows ships: WinHTTP for https:// and CNG (bcrypt) for SHA-256, both loaded
// on first use so the DLL's import table stays kernel32 + the C runtime. file:/// URLs read a local file (for
// testing a registry before it is published). Blocking: call from a worker thread, never the game thread.
#pragma once
#include <string>

namespace net {

// Registry files and host updates are pinned by hash, so this only stops a broken or endless download from filling
// memory: big models and maps (10 MB and more) fit well inside it.
constexpr size_t kMaxDownload = 256 * 1024 * 1024;

// The body of a 200 response (or the file), up to kMaxDownload bytes; false with `error` otherwise.
bool Fetch(const std::string& url, std::string& body, std::string& error);

std::string Sha256(const std::string& data);    // lowercase hex, "" if CNG is unavailable

// One HTTPS request for a plugin (pluginnet.cpp decides whether it may be made): GET or POST, the extra request headers
// as "Name: value" lines ending in CR LF, never following a redirect (a redirect could lead anywhere: it is answered as its own
// status), at most `maxResponse` bytes of body kept. Any status is a result (`status`); false only when no response
// came (`error` says why). Blocking: call from a worker thread.
struct Request {
    std::string method = "GET", url, headers, body;
    int timeoutMs = 10000;
    size_t maxResponse = 1024 * 1024;
};
bool Send(const Request& request, int& status, std::string& body, std::string& error);

}  // namespace net
