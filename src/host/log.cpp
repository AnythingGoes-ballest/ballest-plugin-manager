#include "log.hpp"

#include <windows.h>

#include <cstdio>
#include <deque>
#include <mutex>

namespace hostlog {
namespace {
std::mutex gLock;
FILE* gFile = nullptr;
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

void Open() {
    std::lock_guard<std::mutex> g(gLock);
    std::wstring dir = DataDir();
    // Create each level; CreateDirectory fails harmlessly when a level exists.
    for (size_t i = 3; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
    }
    gFile = _wfopen((dir + L"\\host.log").c_str(), L"w");
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
    if (!gFile) return;
    std::fprintf(gFile, "%s\n", line.c_str());
    std::fflush(gFile);
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
