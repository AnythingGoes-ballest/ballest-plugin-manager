#include "log.hpp"

#include <windows.h>

#include <cstdio>
#include <deque>
#include <mutex>

namespace hostlog {
namespace {
std::mutex gLock;
HANDLE gFile = INVALID_HANDLE_VALUE;
constexpr size_t kLinesKept = 1000;
std::deque<std::string> gRecent;
size_t gDropped = 0;             // lines no longer in gRecent
}

std::wstring DataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = n ? std::wstring(buf, n) : L".";
    dir += L"\\Ballest\\Saved\\PluginManager";
    return dir;
}

// The log is written through kernel32 (CreateFileW, WriteFile), not the C runtime, and never under gLock while a
// file call runs. A sandboxed copy hooks the runtime's file imports before the log opens, and a hook that refuses a
// path logs the refusal, here, through gLock. Under Wine the runtime holds one lock over every open, the hook ran
// inside it, and with the log opened by the runtime under gLock that was a deadlock of the whole process's file I/O
// (measured 2026-11-03, Proton Experimental: the game thread's first fopen never returned). The host's own imports
// are never hooked, so this path can't call back into the sandbox.
void Open() {
    std::wstring dir = DataDir();
    // Create each level; CreateDirectory fails harmlessly when a level exists.
    for (size_t i = 3; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
    }
    const HANDLE file = CreateFileW((dir + L"\\host.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::lock_guard<std::mutex> g(gLock);
    gFile = file;
}

void Write(const char* level, const std::string& source, const std::string& message) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char head[96];
    std::snprintf(head, sizeof head, "[%02d:%02d:%02d.%03d] [%s] [%s] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level,
                  source.c_str());
    const std::string line = head + message;
    std::lock_guard<std::mutex> g(gLock);
    gRecent.push_back(line);
    if (gRecent.size() > kLinesKept) {
        gRecent.pop_front();
        ++gDropped;
    }
    if (gFile == INVALID_HANDLE_VALUE) return;
    const std::string out = line + "\n";
    DWORD written = 0;
    WriteFile(gFile, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
}

size_t LineCount() {
    std::lock_guard<std::mutex> g(gLock);
    return gDropped + gRecent.size();
}

std::string Line(size_t index) {
    std::lock_guard<std::mutex> g(gLock);
    return index >= gDropped && index - gDropped < gRecent.size() ? gRecent[index - gDropped] : std::string();
}

std::string Hex(unsigned long long v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", v);
    return buf;
}
}  // namespace hostlog
