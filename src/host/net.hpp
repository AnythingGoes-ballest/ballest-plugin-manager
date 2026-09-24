// Downloads and hashes, with what Windows ships: WinHTTP for https:// and CNG (bcrypt) for SHA-256, both loaded
// on first use so the DLL's import table stays kernel32 + the C runtime. file:/// URLs read a local file (for
// testing a registry before it is published). Blocking: call from a worker thread, never the game thread.
#pragma once
#include <string>

namespace net {

constexpr size_t kMaxDownload = 8 * 1024 * 1024;

// The body of a 200 response (or the file), up to kMaxDownload bytes; false with `error` otherwise.
bool Fetch(const std::string& url, std::string& body, std::string& error);

std::string Sha256(const std::string& data);    // lowercase hex, "" if CNG is unavailable

}  // namespace net
