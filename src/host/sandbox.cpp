#include "sandbox.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <string>
#include <vector>

#include "log.hpp"
#include "race.hpp"

namespace sandbox {
namespace {

bool gOn = false;
bool gImportsDone = false, gSteamDone = false;
std::string gImportReport, gWindowReport;

// --- import table hooks (the game exe's own imports) ----------------------------------------------------------------

// A module's import slot for a function, by name or, as WS2_32 is often imported, by ordinal (0: by name only).
// Measured 2026-10-03: the game exe imports connect (4), sendto (20) and gethostbyname (52) by ordinal; they used to be
// skipped here.
void** FindImportIn(HMODULE module, const char* dll, const char* function, WORD ordinal = 0) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    if (!base) return nullptr;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
                if (ordinal && IMAGE_ORDINAL64(names->u1.Ordinal) == ordinal) return reinterpret_cast<void**>(&slots->u1.Function);
                continue;
            }
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

void** FindImport(const char* dll, const char* function, WORD ordinal = 0) {
    return FindImportIn(GetModuleHandleW(nullptr), dll, function, ordinal);
}

bool Patch(void** slot, void* value, void** original) {
    DWORD old = 0;
    if (!slot || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (original) *original = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// The game exe delay-loads steam_api64.dll (it isn't in the import table above): its delay-load table, same lookup.
struct DelayDescriptor {                // IMAGE_DELAYLOAD_DESCRIPTOR (x64: all RVAs)
    DWORD attributes, dllName, moduleHandle, addressTable, nameTable, boundTable, unloadTable, timeStamp;
};

void** FindDelayImport(const char* dll, const char* function) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<DelayDescriptor*>(base + dir.VirtualAddress); d->dllName; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->dllName), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->nameTable);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->addressTable);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

std::atomic<long> gBlockedLookups{0}, gBlockedWrites{0};
std::atomic<bool> gLoggedLookup{false}, gLoggedWrite{false};

using GetAddrInfo = INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
GetAddrInfo gGetAddrInfo = nullptr;
CreateFileWFn gCreateFileW = nullptr;
void** gGetAddrInfoSlot = nullptr;
void** gCreateFileSlot = nullptr;

// Every name lookup fails except this computer's own (nothing in the game needs that, but it costs nothing).
INT WSAAPI BlockedGetAddrInfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result) {
    if (node && (_stricmp(node, "localhost") == 0 || std::strcmp(node, "127.0.0.1") == 0 || std::strcmp(node, "::1") == 0))
        return gGetAddrInfo(node, service, hints, result);
    ++gBlockedLookups;
    if (!gLoggedLookup.exchange(true)) hostlog::Info(std::string("sandbox: blocked the game's internet lookup of ") + (node ? node : "?"));
    if (result) *result = nullptr;
    WSASetLastError(WSAHOST_NOT_FOUND);
    return WSAHOST_NOT_FOUND;
}

// Name lookups are not the only way out (audit 2026-10-03): a URL or proxy given as an IP address needs no lookup, a
// proxy on localhost forwards anything, and Socket.IO connects through ConnectEx, fetched at runtime (WSAIoctl). So
// every connection and datagram is refused, loopback included (a local proxy is a way out too), in the game exe and in
// Steam's client DLL inside the process (its P2P and networking sockets). The host's own downloads (net.cpp) are made
// from this DLL, not through these import slots.
std::atomic<long> gBlockedConnections{0};
std::atomic<bool> gLoggedConnection{false};

void NoteConnection(const char* how) {
    ++gBlockedConnections;
    if (!gLoggedConnection.exchange(true)) hostlog::Info(std::string("sandbox: blocked the game's network ") + how);
}

int WSAAPI BlockedConnect(SOCKET, const sockaddr*, int) {
    NoteConnection("connection");
    WSASetLastError(WSAEACCES);
    return SOCKET_ERROR;
}
int WSAAPI BlockedWSAConnect(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS) {
    NoteConnection("connection (WSAConnect)");
    WSASetLastError(WSAEACCES);
    return SOCKET_ERROR;
}
int WSAAPI BlockedSendTo(SOCKET, const char*, int, int, const sockaddr*, int) {
    NoteConnection("datagram");
    WSASetLastError(WSAEACCES);
    return SOCKET_ERROR;
}
int WSAAPI BlockedWSASendTo(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE) {
    NoteConnection("datagram (WSASendTo)");
    WSASetLastError(WSAEACCES);
    return SOCKET_ERROR;
}
int WSAAPI BlockedWSASendMsg(SOCKET, LPWSAMSG, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE) {
    NoteConnection("datagram (WSASendMsg)");
    WSASetLastError(WSAEACCES);
    return SOCKET_ERROR;
}
hostent* WSAAPI BlockedGetHostByName(const char* name) {
    ++gBlockedLookups;
    if (!gLoggedLookup.exchange(true)) hostlog::Info(std::string("sandbox: blocked the game's internet lookup of ") + (name ? name : "?"));
    WSASetLastError(WSAHOST_NOT_FOUND);
    return nullptr;
}
BOOL PASCAL BlockedConnectEx(SOCKET, const sockaddr*, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED) {
    NoteConnection("connection (ConnectEx)");
    WSASetLastError(WSAEACCES);
    return FALSE;
}
using WSAIoctlFn = int(WSAAPI*)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
WSAIoctlFn gWSAIoctl = nullptr;
const GUID kConnectEx = {0x25a207b9, 0xddf3, 0x4660, {0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e}};    // WSAID_CONNECTEX
int WSAAPI FilteredWSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD inSize, LPVOID out, DWORD outSize, LPDWORD returned,
                            LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE done) {
    const int r = gWSAIoctl(s, code, in, inSize, out, outSize, returned, overlapped, done);
    if (r == 0 && code == SIO_GET_EXTENSION_FUNCTION_POINTER && in && inSize >= sizeof(GUID) && out && outSize >= sizeof(void*) &&
        std::memcmp(in, &kConnectEx, sizeof(GUID)) == 0)
        *static_cast<void**>(out) = reinterpret_cast<void*>(&BlockedConnectEx);
    return r;
}

// Every WS2_32 way out in one module's imports; how many of them it imports and were blocked, and whether any failed.
struct NetHooks {
    int found = 0, blocked = 0;
};
NetHooks BlockNetwork(HMODULE module) {
    NetHooks n;
    auto one = [&](const char* name, WORD ordinal, void* hook, void** original = nullptr) {
        void** slot = FindImportIn(module, "WS2_32.dll", name, ordinal);
        if (!slot) return;
        ++n.found;
        if (*slot == hook || Patch(slot, hook, original)) ++n.blocked;
    };
    void* ignored = nullptr;
    one("getaddrinfo", 0, reinterpret_cast<void*>(&BlockedGetAddrInfo), gGetAddrInfo ? &ignored : reinterpret_cast<void**>(&gGetAddrInfo));
    one("gethostbyname", 52, reinterpret_cast<void*>(&BlockedGetHostByName));
    one("connect", 4, reinterpret_cast<void*>(&BlockedConnect));
    one("WSAConnect", 0, reinterpret_cast<void*>(&BlockedWSAConnect));
    one("sendto", 20, reinterpret_cast<void*>(&BlockedSendTo));
    one("WSASendTo", 0, reinterpret_cast<void*>(&BlockedWSASendTo));
    one("WSASendMsg", 0, reinterpret_cast<void*>(&BlockedWSASendMsg));
    one("WSAIoctl", 0, reinterpret_cast<void*>(&FilteredWSAIoctl), gWSAIoctl ? &ignored : reinterpret_cast<void**>(&gWSAIoctl));
    return n;
}
std::string gNetReport;                 // per module, for Status
bool gNetComplete = true;
void BlockNetworkIn(const wchar_t* name, HMODULE module) {
    const NetHooks n = BlockNetwork(module);
    gNetComplete = gNetComplete && n.blocked == n.found;
    const std::wstring w(name);
    gNetReport += (gNetReport.empty() ? "" : ", ") + std::string(w.begin(), w.end()) + " " + std::to_string(n.blocked) + "/" + std::to_string(n.found);
}

std::wstring Normal(const std::wstring& path) {
    wchar_t full[1024];
    const DWORD n = GetFullPathNameW(path.c_str(), 1024, full, nullptr);     // ".." resolved, as the file system will
    std::wstring p = n > 0 && n < 1024 ? std::wstring(full, n) : path;
    for (auto& c : p) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

std::wstring gPlayerSaved;              // the player's real %LOCALAPPDATA%\Ballest\Saved (from Windows, not the variable)

// A path as the file system will see it: the long form of its folder (8.3 short names like SAVEGA~1 resolved).
std::wstring LongForm(const std::wstring& path) {
    std::wstring p = Normal(path);
    const size_t cut = p.find_last_of(L'\\');
    if (cut == std::wstring::npos) return p;
    wchar_t buffer[1024];
    const DWORD n = GetLongPathNameW(p.substr(0, cut).c_str(), buffer, 1024);
    if (n == 0 || n >= 1024) return p;
    return Normal(std::wstring(buffer, n) + p.substr(cut));
}

// The copy's own user folder (the game's -userdir=, where its Saved goes), when it's somewhere of its own: records
// written there are the copy's, not the player's (tools/test_instance.py seeds it with a copy of the player's).
std::wstring gOwnUserDir;

void FindOwnUserDir() {
    const std::wstring line = GetCommandLineW();
    std::wstring lower = line;
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    const size_t at = lower.find(L"-userdir=");
    if (at == std::wstring::npos) return;
    size_t i = at + 9, end;
    if (i < line.size() && line[i] == L'"') end = line.find(L'"', ++i);
    else end = line.find(L' ', i);
    const std::wstring dir = Normal(line.substr(i, end == std::wstring::npos ? std::wstring::npos : end - i));
    // never the player's own folder (%LOCALAPPDATA%\Ballest, from Windows: the variable is the copy's own)
    PWSTR local = nullptr;
    if (dir.size() < 4 || SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local) != S_OK) return;
    const std::wstring player = Normal(std::wstring(local) + L"\\Ballest");
    CoTaskMemFree(local);
    gPlayerSaved = LongForm(player + L"\\Saved\\x");
    gPlayerSaved = gPlayerSaved.substr(0, gPlayerSaved.size() - 2);
    if (dir == player || dir.rfind(player + L"\\", 0) == 0 || player.rfind(dir + L"\\", 0) == 0) return;
    gOwnUserDir = dir;
}

bool UnderRecords(LPCWSTR path) {
    if (!path) return false;
    const std::wstring p = LongForm(path);
    if (!gOwnUserDir.empty() && p.rfind(gOwnUserDir + L"\\", 0) == 0) return false;
    // Anything in the player's own Saved folder (saves, ghosts, maps and playlists, which Steam Cloud syncs, config),
    // except the launcher's readiness files the copy writes there whatever -userdir says (measured; the tools read them)
    if (!gPlayerSaved.empty() && p.rfind(gPlayerSaved + L"\\", 0) == 0) return p.rfind(gPlayerSaved + L"\\ballestlauncher\\", 0) != 0;
    return p.find(L"\\saved\\savegames\\") != std::wstring::npos || p.find(L"\\saved\\ghosts\\") != std::wstring::npos;
}

bool Refuse(LPCWSTR path, const char* how) {
    if (!UnderRecords(path)) return false;
    ++gBlockedWrites;
    if (!gLoggedWrite.exchange(true)) {
        const std::wstring w(path);
        hostlog::Info(std::string("sandbox: blocked the game ") + how + " its records (" + std::string(w.begin(), w.end()) + ")");
    }
    SetLastError(ERROR_ACCESS_DENIED);
    return true;
}
std::wstring Wide(LPCSTR path) {
    if (!path) return L"";
    wchar_t buffer[1024];
    const int n = MultiByteToWideChar(CP_ACP, 0, path, -1, buffer, 1024);
    return n > 0 ? std::wstring(buffer) : L"";
}

using CreateFile2Fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, void*);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using MoveFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
using MoveFileExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using MoveFileExAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, DWORD);
using ReplaceFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
using PathFn = BOOL(WINAPI*)(LPCWSTR);
using SetAttributesFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
using CopyFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
using WriteProfileFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
CreateFile2Fn gCreateFile2 = nullptr;
CreateFileAFn gCreateFileA = nullptr;
MoveFileWFn gMoveFileW = nullptr;
MoveFileExWFn gMoveFileExW = nullptr;
MoveFileExAFn gMoveFileExA = nullptr;
ReplaceFileWFn gReplaceFileW = nullptr;
PathFn gDeleteFileW = nullptr, gRemoveDirectoryW = nullptr;
SetAttributesFn gSetFileAttributesW = nullptr;
CopyFileWFn gCopyFileW = nullptr;
WriteProfileFn gWritePrivateProfileStringW = nullptr;

bool WritingAccess(DWORD access, DWORD disposition) {
    const DWORD writes = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER;
    return (access & writes) || disposition == CREATE_ALWAYS || disposition == CREATE_NEW || disposition == TRUNCATE_EXISTING;
}
HANDLE WINAPI BlockedCreateFile2(LPCWSTR path, DWORD access, DWORD share, DWORD disposition, void* params) {
    if (WritingAccess(access, disposition) && Refuse(path, "writing")) return INVALID_HANDLE_VALUE;
    return gCreateFile2(path, access, share, disposition, params);
}
HANDLE WINAPI BlockedCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags,
                                 HANDLE templateFile) {
    if (WritingAccess(access, disposition) && Refuse(Wide(path).c_str(), "writing")) return INVALID_HANDLE_VALUE;
    return gCreateFileA(path, access, share, security, disposition, flags, templateFile);
}
BOOL WINAPI BlockedMoveFileW(LPCWSTR from, LPCWSTR to) {
    if (Refuse(from, "moving") || Refuse(to, "moving into")) return FALSE;
    return gMoveFileW(from, to);
}
BOOL WINAPI BlockedMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    if (Refuse(from, "moving") || (to && Refuse(to, "moving into"))) return FALSE;
    return gMoveFileExW(from, to, flags);
}
BOOL WINAPI BlockedMoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    if (Refuse(Wide(from).c_str(), "moving") || (to && Refuse(Wide(to).c_str(), "moving into"))) return FALSE;
    return gMoveFileExA(from, to, flags);
}
BOOL WINAPI BlockedReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags, LPVOID a, LPVOID b) {
    if (Refuse(replaced, "replacing") || (backup && Refuse(backup, "backing up"))) return FALSE;
    return gReplaceFileW(replaced, replacement, backup, flags, a, b);
}
BOOL WINAPI BlockedDeleteFileW(LPCWSTR path) {
    if (Refuse(path, "deleting")) return FALSE;
    return gDeleteFileW(path);
}
BOOL WINAPI BlockedRemoveDirectoryW(LPCWSTR path) {
    if (Refuse((std::wstring(path ? path : L"") + L"\\x").c_str(), "removing a folder of")) return FALSE;
    return gRemoveDirectoryW(path);
}
BOOL WINAPI BlockedSetFileAttributesW(LPCWSTR path, DWORD attributes) {
    if (Refuse(path, "changing")) return FALSE;
    return gSetFileAttributesW(path, attributes);
}
BOOL WINAPI BlockedCopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists) {
    if (Refuse(to, "copying into")) return FALSE;
    return gCopyFileW(from, to, failIfExists);
}
BOOL WINAPI BlockedWritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file) {
    if (file && Refuse(file, "writing")) return FALSE;
    return gWritePrivateProfileStringW(section, key, value, file);
}

// The file functions of one module's imports (from KERNEL32 or the api-ms-win-core-file sets the C runtime uses):
// imported and hooked, imported and failed.
struct FileHooks {
    int found = 0, blocked = 0;
};
HANDLE WINAPI BlockedCreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
FileHooks BlockFiles(HMODULE module) {
    FileHooks f;
    static const char* const kSets[] = {"KERNEL32.dll", "api-ms-win-core-file-l1-1-0.dll", "api-ms-win-core-file-l1-2-0.dll",
                                        "api-ms-win-core-file-l2-1-0.dll", "api-ms-win-core-file-l1-2-1.dll", "api-ms-win-core-file-l1-2-2.dll"};
    auto one = [&](const char* name, void* hook, void** original) {
        for (const char* set : kSets) {
            void** slot = FindImportIn(module, set, name);
            if (!slot) continue;
            ++f.found;
            void* ignored = nullptr;
            if (*slot == hook || Patch(slot, hook, *original ? &ignored : original)) ++f.blocked;
        }
    };
    one("CreateFileW", reinterpret_cast<void*>(&BlockedCreateFileW), reinterpret_cast<void**>(&gCreateFileW));
    one("CreateFile2", reinterpret_cast<void*>(&BlockedCreateFile2), reinterpret_cast<void**>(&gCreateFile2));
    one("CreateFileA", reinterpret_cast<void*>(&BlockedCreateFileA), reinterpret_cast<void**>(&gCreateFileA));
    one("MoveFileW", reinterpret_cast<void*>(&BlockedMoveFileW), reinterpret_cast<void**>(&gMoveFileW));
    one("MoveFileExW", reinterpret_cast<void*>(&BlockedMoveFileExW), reinterpret_cast<void**>(&gMoveFileExW));
    one("MoveFileExA", reinterpret_cast<void*>(&BlockedMoveFileExA), reinterpret_cast<void**>(&gMoveFileExA));
    one("ReplaceFileW", reinterpret_cast<void*>(&BlockedReplaceFileW), reinterpret_cast<void**>(&gReplaceFileW));
    one("DeleteFileW", reinterpret_cast<void*>(&BlockedDeleteFileW), reinterpret_cast<void**>(&gDeleteFileW));
    one("RemoveDirectoryW", reinterpret_cast<void*>(&BlockedRemoveDirectoryW), reinterpret_cast<void**>(&gRemoveDirectoryW));
    one("SetFileAttributesW", reinterpret_cast<void*>(&BlockedSetFileAttributesW), reinterpret_cast<void**>(&gSetFileAttributesW));
    one("CopyFileW", reinterpret_cast<void*>(&BlockedCopyFileW), reinterpret_cast<void**>(&gCopyFileW));
    one("WritePrivateProfileStringW", reinterpret_cast<void*>(&BlockedWritePrivateProfileStringW),
        reinterpret_cast<void**>(&gWritePrivateProfileStringW));
    return f;
}
std::string gFileReport;
bool gFileComplete = true;
void BlockFilesIn(const wchar_t* name, HMODULE module) {
    if (!module) return;
    const FileHooks f = BlockFiles(module);
    gFileComplete = gFileComplete && f.blocked == f.found;
    const std::wstring w(name);
    gFileReport += (gFileReport.empty() ? "" : ", ") + std::string(w.begin(), w.end()) + " " + std::to_string(f.blocked) + "/" + std::to_string(f.found);
}

// --- the window: a test copy opens behind everything and never takes focus from the player ------------------------

using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using SetWindowPlacementFn = BOOL(WINAPI*)(HWND, const WINDOWPLACEMENT*);
using SetForegroundWindowFn = BOOL(WINAPI*)(HWND);
using SetActiveWindowFn = HWND(WINAPI*)(HWND);
using SetFocusFn = HWND(WINAPI*)(HWND);
SetFocusFn gSetFocus = nullptr;
ShowWindowFn gShowWindow = nullptr;
SetWindowPosFn gSetWindowPos = nullptr;
SetWindowPlacementFn gSetWindowPlacement = nullptr;
SetForegroundWindowFn gSetForegroundWindow = nullptr;
SetActiveWindowFn gSetActiveWindow = nullptr;
std::atomic<long> gBlockedActivations{0};

int Quiet(int show) {
    switch (show) {
        case SW_SHOWNORMAL: case SW_SHOWMAXIMIZED: case SW_RESTORE: case SW_SHOWDEFAULT: return SW_SHOWNOACTIVATE;
        case SW_SHOW: return SW_SHOWNA;
        case SW_SHOWMINIMIZED: return SW_SHOWMINNOACTIVE;
        default: return show;
    }
}

BOOL WINAPI QuietShowWindow(HWND window, int show) {
    const int quiet = Quiet(show);
    if (quiet != show) ++gBlockedActivations;
    // Never activated at all, not even by Windows itself (measured: with four copies up, one became the foreground
    // window without calling any of these, as Windows picks a next window when the front one goes). Input reaches
    // it anyway: the test channel posts it to the window.
    if (!GetParent(window)) SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    const BOOL was = gShowWindow(window, quiet);
    if (quiet != SW_HIDE && !GetParent(window))
        gSetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    return was;
}

BOOL WINAPI QuietSetWindowPos(HWND window, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!(flags & SWP_NOZORDER) && (after == HWND_TOP || after == HWND_TOPMOST || after == nullptr)) after = HWND_BOTTOM;
    if (!(flags & SWP_NOACTIVATE)) ++gBlockedActivations;
    return gSetWindowPos(window, after, x, y, cx, cy, flags | SWP_NOACTIVATE);
}

BOOL WINAPI QuietSetWindowPlacement(HWND window, const WINDOWPLACEMENT* placement) {
    if (!placement) return gSetWindowPlacement(window, placement);
    WINDOWPLACEMENT copy = *placement;
    copy.showCmd = static_cast<UINT>(Quiet(static_cast<int>(copy.showCmd)));
    return gSetWindowPlacement(window, &copy);
}

BOOL WINAPI QuietSetForegroundWindow(HWND) {
    ++gBlockedActivations;
    return TRUE;
}

// Keyboard focus inside the copy's own window is fine while the player has put it in front; from behind, setting it
// would activate the window.
HWND WINAPI QuietSetFocus(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid == GetCurrentProcessId()) return gSetFocus(w);
    ++gBlockedActivations;
    return GetFocus();
}

HWND WINAPI QuietSetActiveWindow(HWND) {
    ++gBlockedActivations;
    return GetActiveWindow();
}

HANDLE WINAPI BlockedCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                 DWORD flags, HANDLE templateFile) {
    if (WritingAccess(access, disposition) && Refuse(path, "writing")) return INVALID_HANDLE_VALUE;
    return gCreateFileW(path, access, share, security, disposition, flags, templateFile);
}

// --- Steam interface methods -------------------------------------------------------------------------------------

struct SteamBlock {
    const char* accessor;       // the flat function returning the interface (this steam_api64.dll's version of it)
    const char* method;         // the flat wrapper of the method
    void* stub = nullptr;
    std::atomic<long> count{0};
    bool installed = false;     // in the flat accessor's object (what the host's own code would call)
    void* original = nullptr;   // the method's code in that object's table, before the block: how it's found in the
                                // game's own objects, which can be OTHER versions of the interface (below)
};
SteamBlock gSteam[] = {
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_UploadLeaderboardScore"},
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_AttachLeaderboardUGC"},
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_SetStatInt32"},
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_SetStatFloat"},
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_UpdateAvgRateStat"},
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_SetAchievement"},
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_ClearAchievement"},
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_IndicateAchievementProgress"},
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_StoreStats"},
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_ResetAllStats"},
    {"SteamAPI_SteamUserStats_v013", "SteamAPI_ISteamUserStats_FindOrCreateLeaderboard"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_FileWrite"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_FileWriteAsync"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_FileShare"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_FileDelete"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_FileForget"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_FileWriteStreamClose"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_SetSyncPlatforms"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_PublishWorkshopFile"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_PublishVideo"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_DeletePublishedFile"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_SubscribePublishedFile"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile"},
    {"SteamAPI_SteamRemoteStorage_v016", "SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_CreateItem"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_StartItemUpdate"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_SubmitItemUpdate"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_DeleteItem"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_SetUserItemVote"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_AddItemToFavorites"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_RemoveItemFromFavorites"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_StartPlaytimeTracking"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_StopPlaytimeTracking"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_SubscribeItem"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_UnsubscribeItem"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_AddDependency"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_RemoveDependency"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_AddAppDependency"},
    {"SteamAPI_SteamUGC_v021", "SteamAPI_ISteamUGC_RemoveAppDependency"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_SetRichPresence"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_ClearRichPresence"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_SetPlayedWith"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_InviteUserToGame"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_ReplyToFriendMessage"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_SendClanChatMessage"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_JoinClanChatRoom"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_LeaveClanChatRoom"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_ActivateGameOverlay"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_ActivateGameOverlayToUser"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_ActivateGameOverlayToStore"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString"},
    {"SteamAPI_SteamFriends_v018", "SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog"},
    {"SteamAPI_SteamHTTP_v003", "SteamAPI_ISteamHTTP_CreateHTTPRequest"},
    {"SteamAPI_SteamHTTP_v003", "SteamAPI_ISteamHTTP_SendHTTPRequest"},
    {"SteamAPI_SteamHTTP_v003", "SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse"},
    {"SteamAPI_SteamUser_v023", "SteamAPI_ISteamUser_GetAuthSessionTicket"},
    {"SteamAPI_SteamUser_v023", "SteamAPI_ISteamUser_GetAuthTicketForWebApi"},
    {"SteamAPI_SteamUser_v023", "SteamAPI_ISteamUser_RequestEncryptedAppTicket"},
    {"SteamAPI_SteamUser_v023", "SteamAPI_ISteamUser_AdvertiseGame"},
    {"SteamAPI_SteamUser_v023", "SteamAPI_ISteamUser_TrackAppUsageEvent"},
    {"SteamAPI_SteamUser_v023", "SteamAPI_ISteamUser_StartVoiceRecording"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_CreateLobby"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_JoinLobby"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_SetLobbyData"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_SetLobbyMemberData"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_SendLobbyChatMsg"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_InviteUserToLobby"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_SetLobbyGameServer"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_SetLobbyJoinable"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_SetLobbyOwner"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_SetLobbyType"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_AddFavoriteGame"},
    {"SteamAPI_SteamMatchmaking_v009", "SteamAPI_ISteamMatchmaking_RemoveFavoriteGame"},
    {"SteamAPI_SteamNetworking_v006", "SteamAPI_ISteamNetworking_SendP2PPacket"},
    {"SteamAPI_SteamNetworking_v006", "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser"},
    {"SteamAPI_SteamNetworking_v006", "SteamAPI_ISteamNetworking_CreateP2PConnectionSocket"},
    {"SteamAPI_SteamNetworking_v006", "SteamAPI_ISteamNetworking_CreateConnectionSocket"},
    {"SteamAPI_SteamNetworking_v006", "SteamAPI_ISteamNetworking_CreateListenSocket"},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress"},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamAPI_ISteamNetworkingSockets_ConnectP2P"},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling"},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer"},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP"},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P"},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP"},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket"},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection"},
    {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamAPI_ISteamNetworkingSockets_SendMessages"},
    {"SteamAPI_SteamNetworkingMessages_SteamAPI_v002", "SteamAPI_ISteamNetworkingMessages_SendMessageToUser"},
    {"SteamAPI_SteamNetworkingMessages_SteamAPI_v002", "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser"},
    {"SteamAPI_SteamHTMLSurface_v005", "SteamAPI_ISteamHTMLSurface_CreateBrowser"},
    {"SteamAPI_SteamScreenshots_v003", "SteamAPI_ISteamScreenshots_WriteScreenshot"},
    {"SteamAPI_SteamScreenshots_v003", "SteamAPI_ISteamScreenshots_AddScreenshotToLibrary"},
    {"SteamAPI_SteamScreenshots_v003", "SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary"},
    {"SteamAPI_SteamScreenshots_v003", "SteamAPI_ISteamScreenshots_TriggerScreenshot"},
    {"SteamAPI_SteamScreenshots_v003", "SteamAPI_ISteamScreenshots_SetLocation"},
    {"SteamAPI_SteamScreenshots_v003", "SteamAPI_ISteamScreenshots_TagUser"},
    {"SteamAPI_SteamScreenshots_v003", "SteamAPI_ISteamScreenshots_TagPublishedFile"},
    {"SteamAPI_SteamParties_v002", "SteamAPI_ISteamParties_CreateBeacon"},
    {"SteamAPI_SteamParties_v002", "SteamAPI_ISteamParties_JoinParty"},
    {"SteamAPI_SteamParties_v002", "SteamAPI_ISteamParties_ChangeNumOpenSlots"},
    {"SteamAPI_SteamParties_v002", "SteamAPI_ISteamParties_OnReservationCompleted"},
    {"SteamAPI_SteamParties_v002", "SteamAPI_ISteamParties_CancelReservation"},
    {"SteamAPI_SteamRemotePlay_v003", "SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite"},
    {"SteamAPI_SteamApps_v008", "SteamAPI_ISteamApps_MarkContentCorrupt"},
    {"SteamAPI_SteamApps_v008", "SteamAPI_ISteamApps_InstallDLC"},
    {"SteamAPI_SteamApps_v008", "SteamAPI_ISteamApps_UninstallDLC"},
    {"SteamAPI_SteamApps_v008", "SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys"},
};
constexpr int kSteamBlocks = static_cast<int>(sizeof gSteam / sizeof gSteam[0]);

// One stub per method, so the log names what was blocked. Every blocked method answers 0: false, an invalid call
// (k_uAPICallInvalid), or no handle. Extra arguments are ignored (the caller cleans up in the x64 convention).
template <int N>
uint64_t Stub(void*, uint64_t, uint64_t, uint64_t) {
    if (gSteam[N].count++ == 0) hostlog::Info(std::string("sandbox: blocked Steam ") + (gSteam[N].method + 9));
    return 0;
}
template <int... N>
void FillStubs(std::integer_sequence<int, N...>) {
    ((gSteam[N].stub = reinterpret_cast<void*>(&Stub<N>)), ...);
}

// The method's byte offset in its interface's table, read from the flat wrapper: mov rax,[rcx] then jmp/call
// [rax+disp8] (FF 60 / FF 50) or [rax+disp32] (FF A0 / FF 90), with an optional REX.W prefix.
int SlotOffset(const uint8_t* code, std::string* bytes) {
    char hex[4];
    for (int i = 0; i < 24; ++i) {
        std::snprintf(hex, sizeof hex, "%02x", code[i]);
        *bytes += hex;
    }
    for (int i = 0; i + 3 <= 32; ++i) {
        if (!(code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x01)) continue;     // mov rax,[rcx]
        for (int j = i + 3; j < i + 40; ++j) {
            // A wrapper ends at ret / int3 padding: past it is the next function (measured 2026-10-03: FileWrite, the
            // first method, is jmp [rax] with no displacement; the scan used to run on into the next wrapper and took
            // FileDelete's slot, so FileWrite was never blocked)
            if (code[j] == 0xC3 || code[j] == 0xCC) break;
            if (code[j] == 0x4C && code[j + 1] == 0x8B && code[j + 2] == 0x10) return 0;     // mov r10,[rax]
            // mov r10,[rax+disp] (4C 8B 50 disp8 / 4C 8B 90 disp32), then jmp r10 (measured: PublishVideo's wrapper)
            if (code[j] == 0x4C && code[j + 1] == 0x8B && (code[j + 2] == 0x50 || code[j + 2] == 0x90)) {
                if (code[j + 2] == 0x50) return code[j + 3];
                int32_t disp = 0;
                std::memcpy(&disp, code + j + 3, sizeof disp);
                return disp;
            }
            int k = j;
            if (code[k] == 0x48) ++k;
            if (code[k] != 0xFF) continue;
            const uint8_t modrm = code[k + 1];
            if (modrm == 0x20 || modrm == 0x10) return 0;                                    // jmp/call [rax]
            if (modrm == 0x60 || modrm == 0x50) return code[k + 2];
            if (modrm == 0xA0 || modrm == 0x90) {
                int32_t disp = 0;
                std::memcpy(&disp, code + k + 2, sizeof disp);
                return disp;
            }
        }
        break;
    }
    return -1;
}

// --- the interfaces the game itself uses ------------------------------------------------------------------------
// Blocking the flat accessors' objects is not enough (measured 2026-10-03: a teleported finish in a slot reached the
// real leaderboard). Steam hands out one object per interface VERSION, each with its own table, and the game asks for
// older versions than this steam_api64.dll's flat API: STEAMUSERSTATS_INTERFACE_VERSION012 (flat: v013),
// STEAMUGC_INTERFACE_VERSION020 (v021), SteamFriends017 (v018). So every interface the game asks Steam for is caught
// where it asks (SteamInternal_FindOrCreateUserInterface, through the exe's delay-load table), and the blocked methods
// are replaced in that object's own table too. A method is found there by its code: the entry that holds the same
// function as the flat accessor's object does for it. One that can't be found is reported (the sandbox is then
// INCOMPLETE), never assumed.

using FindOrCreateFn = void* (*)(int32_t, const char*);
void** gFindOrCreateSlot = nullptr;
FindOrCreateFn gFindOrCreate = nullptr;     // steam_api64.dll's own, once it's loaded
FindOrCreateFn gFindOrCreateThunk = nullptr;  // the exe's delay-load thunk (loads the DLL, then unhooks us: last resort)

struct GameInterface {
    std::string version;
    void* object;
    int blocked = 0, missing = 0;           // its family's methods replaced in its table / not found there
    bool done = false;
};
std::vector<GameInterface> gGameInterfaces;
SRWLOCK gInterfaceLock = SRWLOCK_INIT;
std::atomic<long> gUnhookedRequests{0};     // asked for before steam_api64.dll was loaded (passed on, not seen)

// The game's version strings of each flat accessor's interface (read from the exe; their spellings differ). A
// prefix that also starts another family's strings ends with the digit that rules it out ("SteamUser0" is not
// SteamUserStats, "SteamMatchMaking0" not SteamMatchMakingServers, "SteamNetworking0" not NetworkingSockets).
}  // namespace
bool Complete();
namespace {
std::string FamilyOf(const char* accessor) {
    static const char* const kFamilies[][2] = {
        {"SteamAPI_SteamUserStats_", "STEAMUSERSTATS_INTERFACE_VERSION"},
        {"SteamAPI_SteamRemoteStorage_", "STEAMREMOTESTORAGE_INTERFACE_VERSION"},
        {"SteamAPI_SteamUGC_", "STEAMUGC_INTERFACE_VERSION"},
        {"SteamAPI_SteamFriends_", "SteamFriends"},
        {"SteamAPI_SteamHTTP_", "STEAMHTTP_INTERFACE_VERSION"},
        {"SteamAPI_SteamUser_", "SteamUser0"},
        {"SteamAPI_SteamMatchmaking_", "SteamMatchMaking0"},
        {"SteamAPI_SteamNetworking_v", "SteamNetworking0"},
        {"SteamAPI_SteamNetworkingSockets_", "SteamNetworkingSockets"},
        {"SteamAPI_SteamNetworkingMessages_", "SteamNetworkingMessages"},
        {"SteamAPI_SteamHTMLSurface_", "STEAMHTMLSURFACE_INTERFACE_VERSION"},
        {"SteamAPI_SteamScreenshots_", "STEAMSCREENSHOTS_INTERFACE_VERSION"},
        {"SteamAPI_SteamParties_", "SteamParties"},
        {"SteamAPI_SteamRemotePlay_", "STEAMREMOTEPLAY_INTERFACE_VERSION"},
        {"SteamAPI_SteamApps_", "STEAMAPPS_INTERFACE_VERSION"},
    };
    for (const auto& f : kFamilies)
        if (std::strncmp(accessor, f[0], std::strlen(f[0])) == 0) return f[1];
    return "?";                                         // matches no game interface: Status shows it as missing
}

bool Executable(const void* p) {
    MEMORY_BASIC_INFORMATION info;
    if (!p || !VirtualQuery(p, &info, sizeof info) || info.State != MEM_COMMIT) return false;
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool Readable(const void* p) {
    MEMORY_BASIC_INFORMATION info;
    return p && VirtualQuery(p, &info, sizeof info) && info.State == MEM_COMMIT &&
           !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

std::atomic<long> gPersonaCount{0};
uint64_t PersonaStub(void*, uint64_t) {
    if (gPersonaCount++ == 0) hostlog::Info("sandbox: blocked Steam ISteamFriends_SetPersonaName");
    return 0;
}
void* const gPersonaStub = reinterpret_cast<void*>(&PersonaStub);

// Steam game servers: never started in a sandbox (their stats interface writes player stats, and their interfaces
// would be objects these hooks never see). Init fails; interface requests get nothing.
std::atomic<long> gBlockedServers{0};
int32_t BlockedGameServerInit(uint32_t, uint16_t, uint16_t, int, const char*, const char*, void*) {
    if (gBlockedServers++ == 0) hostlog::Info("sandbox: blocked starting a Steam game server");
    return 1;                                           // k_ESteamAPIInitResult_FailedGeneric
}
void* BlockedGameServerInterface(int32_t, const char* version) {
    if (gBlockedServers++ == 0) hostlog::Info(std::string("sandbox: blocked a Steam game-server interface: ") + (version ? version : "?"));
    return nullptr;
}

bool InstallSteamLocked();

// The blocks of one game interface, once the flat accessors' tables have been read (gSteamDone). Under the lock.
void BlockGameInterface(GameInterface& gi) {
    if (gi.done || !gSteamDone) return;
    gi.done = true;
    void** table = *reinterpret_cast<void***>(gi.object);
    for (auto& b : gSteam) {
        if (gi.version.rfind(FamilyOf(b.accessor), 0) != 0) continue;
        if (!b.original) {
            ++gi.missing;
            continue;
        }
        int found = -1, matches = 0;
        for (int i = 0; i < 400 && Readable(table + i) && Executable(table[i]); ++i)
            if (table[i] == b.original || table[i] == b.stub) {     // the stub: the same table as the flat object's
                found = i;
                ++matches;
            }
        if (matches == 1 && (table[found] == b.stub || Patch(table + found, b.stub, nullptr))) {
            ++gi.blocked;
        } else {
            ++gi.missing;
            char detail[160];
            std::snprintf(detail, sizeof detail, "; looked for %p, table %p, first entries %p %p %p", b.original,
                          static_cast<void*>(table), table[0], table[1], table[2]);
            hostlog::Error("sandbox: " + std::string(b.method + 9) + " not found in the game's " + gi.version + " (" +
                           std::to_string(matches) + " matches" + detail + "); NOT BLOCKED");
        }
    }
    if (gi.version == "SteamFriends017") {             // SetPersonaName (renames the player's Steam profile): slot 1
        bool inNewer = false;
        if (HMODULE dll = GetModuleHandleW(L"steam_api64.dll"))
            if (auto accessor = reinterpret_cast<void* (*)()>(GetProcAddress(dll, "SteamAPI_SteamFriends_v018")))
                if (void* flat = accessor()) {
                    void** newer = *reinterpret_cast<void***>(flat);
                    for (int i = 0; i < 400 && Readable(newer + i) && Executable(newer[i]); ++i) inNewer = inNewer || newer[i] == table[1];
                }
        if (!inNewer && (table[1] == gPersonaStub || Patch(table + 1, gPersonaStub, nullptr))) {
            ++gi.blocked;
        } else {
            ++gi.missing;
            hostlog::Error("sandbox: SteamFriends017 slot 1 is also in v018, so it isn't SetPersonaName as expected; NOT BLOCKED");
        }
    }
    hostlog::Info("sandbox: the game's " + gi.version + ": " + std::to_string(gi.blocked) + " blocked" +
                  (gi.missing ? ", " + std::to_string(gi.missing) + " NOT FOUND" : ""));
}

// --- the upload guard, in a player's game ---------------------------------------------------------------------------
// The game's own leaderboard uploads (its score, and the replay attached to it) go through, except for a run the host
// has touched (race::RunTainted: a test command that changes the game, the ball moved, practice, game time not at
// normal speed). Pass-through otherwise: the guard sits in the same table slots the sandbox blocks, found the same way.
using UploadFn = uint64_t (*)(void*, uint64_t, int, int32_t, const int32_t*, int);
using AttachFn = uint64_t (*)(void*, uint64_t, uint64_t);
UploadFn gUploadReal = nullptr;
AttachFn gAttachReal = nullptr;
void* gUploadFlat = nullptr;            // the flat accessor object's code for each: how they're found in the game's
void* gAttachFlat = nullptr;
bool gGuardReady = false;
std::atomic<long> gGuardRefused{0};

uint64_t GuardedUpload(void* self, uint64_t board, int method, int32_t score, const int32_t* details, int count) {
    std::string why;
    if (race::RunTainted(&why)) {
        ++gGuardRefused;
        hostlog::Warn("upload guard: not sending this run's score to the leaderboard (" + why + ")");
        return 0;                       // k_uAPICallInvalid: as if Steam had refused it
    }
    hostlog::Info("upload guard: score " + std::to_string(score) + " goes to the leaderboard (the run wasn't touched)");
    return gUploadReal(self, board, method, score, details, count);
}

uint64_t GuardedAttach(void* self, uint64_t board, uint64_t ugc) {
    std::string why;
    if (race::RunTainted(&why)) {
        ++gGuardRefused;
        hostlog::Warn("upload guard: not attaching this run's replay to the leaderboard (" + why + ")");
        return 0;
    }
    return gAttachReal(self, board, ugc);
}

// The guard in one of the game's leaderboard interfaces (under the interface lock).
void GuardGameInterface(GameInterface& gi) {
    if (gi.done || !gGuardReady || gi.version.rfind("STEAMUSERSTATS_INTERFACE_VERSION", 0) != 0) return;
    gi.done = true;
    void** table = *reinterpret_cast<void***>(gi.object);
    auto place = [&](void* flat, void* guard, void** real) {
        int found = -1, matches = 0;
        for (int i = 0; i < 400 && Readable(table + i) && Executable(table[i]); ++i)
            if (table[i] == flat || table[i] == guard) {
                found = i;
                ++matches;
            }
        if (matches != 1) return false;
        if (table[found] == guard) return true;
        void* original = nullptr;
        if (!Patch(table + found, guard, &original)) return false;
        if (!*real) *real = original;
        return true;
    };
    const bool upload = place(gUploadFlat, reinterpret_cast<void*>(&GuardedUpload), reinterpret_cast<void**>(&gUploadReal));
    const bool attach = place(gAttachFlat, reinterpret_cast<void*>(&GuardedAttach), reinterpret_cast<void**>(&gAttachReal));
    gi.blocked = (upload ? 1 : 0) + (attach ? 1 : 0);
    gi.missing = 2 - gi.blocked;
    if (gi.missing) hostlog::Error("upload guard: NOT in the game's " + gi.version + " (Steam changed?); runs the host touched could reach leaderboards");
    else hostlog::Info("upload guard: on in the game's " + gi.version);
}

// In a test copy the guard sits in front of the sandbox's own block of the same two methods (guard, then stub), so the
// guard's decisions can be tested on a real finish without anything reaching Steam either way.
void ChainGuard(GameInterface& gi) {
    if (gi.version.rfind("STEAMUSERSTATS_INTERFACE_VERSION", 0) != 0) return;
    void** table = *reinterpret_cast<void***>(gi.object);
    for (int i = 0; i < 400 && Readable(table + i) && Executable(table[i]); ++i) {
        if (table[i] == gSteam[0].stub && Patch(table + i, reinterpret_cast<void*>(&GuardedUpload), nullptr))
            gUploadReal = reinterpret_cast<UploadFn>(gSteam[0].stub);
        else if (table[i] == gSteam[1].stub && Patch(table + i, reinterpret_cast<void*>(&GuardedAttach), nullptr))
            gAttachReal = reinterpret_cast<AttachFn>(gSteam[1].stub);
    }
}

void* HookedFindOrCreate(int32_t user, const char* version) {
    if (!gFindOrCreate)
        if (HMODULE dll = GetModuleHandleW(L"steam_api64.dll"))
            gFindOrCreate = reinterpret_cast<FindOrCreateFn>(GetProcAddress(dll, "SteamInternal_FindOrCreateUserInterface"));
    if (!gFindOrCreate) {               // the game always loads the DLL first (measured); if not, it still gets Steam
        ++gUnhookedRequests;
        hostlog::Error(std::string("sandbox: the game asked for ") + (version ? version : "?") +
                       " before steam_api64.dll was loaded; its interfaces are NOT BLOCKED");
        return gFindOrCreateThunk ? gFindOrCreateThunk(user, version) : nullptr;
    }
    void* object = gFindOrCreate(user, version);
    // Not installed from here: the game asks from inside Steam's interface setup (SteamInternal_ContextInit), and the
    // install reads the flat accessors, which go through that setup again: measured, it hung the game at launch. The
    // init thread installs within 20 ms of Steam starting, before the game has a menu.
    if (object && version) {
        AcquireSRWLockExclusive(&gInterfaceLock);
        bool known = false;
        for (const auto& gi : gGameInterfaces) known = known || gi.object == object;
        bool added = false;
        if (!known) {
            gGameInterfaces.push_back({version, object});
            if (gOn) {
                BlockGameInterface(gGameInterfaces.back());
                if (gSteamDone) ChainGuard(gGameInterfaces.back());
            } else {
                GuardGameInterface(gGameInterfaces.back());
            }
            added = gSteamDone;
        }
        ReleaseSRWLockExclusive(&gInterfaceLock);
        // The verdict again whenever the game's interfaces change after the install (the install can finish before
        // the game has asked for any: measured). tools/test_instance.py waits for "status on, complete".
        static std::atomic<bool> reportedComplete{false};
        if (gOn && added && !reportedComplete && Complete()) {
            reportedComplete = true;
            hostlog::Info("sandbox: status " + Status());
        }
    }
    return object;
}

}  // namespace

bool MuteAudio() {
    if (!gOn) return false;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool muted = false;
    IMMDeviceEnumerator* devices = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager* sessions = nullptr;
    ISimpleAudioVolume* volume = nullptr;
    // The process's own session on the default output device: the one the game's audio plays in (it doesn't name one).
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&devices))) &&
        SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&sessions))) &&
        SUCCEEDED(sessions->GetSimpleAudioVolume(nullptr, FALSE, &volume)))
        muted = SUCCEEDED(volume->SetMute(TRUE, nullptr));
    if (volume) volume->Release();
    if (sessions) sessions->Release();
    if (device) device->Release();
    if (devices) devices->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return muted;
}

bool On() { return gOn; }

bool Flagged() {
    const std::wstring dir = hostlog::DataDir();
    return GetFileAttributesW((dir + L"\\sandbox.txt").c_str()) != INVALID_FILE_ATTRIBUTES ||
           GetFileAttributesW((dir + L"\\ai_mode.txt").c_str()) != INVALID_FILE_ATTRIBUTES;     // its older name
}

std::wstring OwnHostPath() { return hostlog::DataDir() + L"\\host\\ballest-host.dll"; }

void InstallEarly() {
    gOn = Flagged();
    if (!gOn) {                         // a player's game: only the watch on the game's Steam interfaces, for the guard
        gFindOrCreateSlot = FindDelayImport("steam_api64.dll", "SteamInternal_FindOrCreateUserInterface");
        if (!Patch(gFindOrCreateSlot, reinterpret_cast<void*>(&HookedFindOrCreate), reinterpret_cast<void**>(&gFindOrCreateThunk)))
            hostlog::Error("upload guard: the game's Steam interfaces can't be watched; runs the host touched could reach leaderboards");
        return;
    }
    FillStubs(std::make_integer_sequence<int, kSteamBlocks>{});
    gGetAddrInfoSlot = FindImport("WS2_32.dll", "getaddrinfo");
    gCreateFileSlot = FindImport("KERNEL32.dll", "CreateFileW");
    BlockNetworkIn(L"game", GetModuleHandleW(nullptr));
    const bool lookups = gGetAddrInfoSlot && *gGetAddrInfoSlot == reinterpret_cast<void*>(&BlockedGetAddrInfo) && gNetComplete;
    FindOwnUserDir();
    BlockFilesIn(L"game", GetModuleHandleW(nullptr));
    for (const wchar_t* runtime : {L"ucrtbase.dll", L"msvcp140.dll", L"vcruntime140.dll"}) BlockFilesIn(runtime, GetModuleHandleW(runtime));
    const bool writes = gCreateFileSlot && *gCreateFileSlot == reinterpret_cast<void*>(&BlockedCreateFileW) && gFileComplete && !gPlayerSaved.empty();
    gFindOrCreateSlot = FindDelayImport("steam_api64.dll", "SteamInternal_FindOrCreateUserInterface");
    const bool interfaces = Patch(gFindOrCreateSlot, reinterpret_cast<void*>(&HookedFindOrCreate),
                                  reinterpret_cast<void**>(&gFindOrCreateThunk));
    const bool servers =
        Patch(FindDelayImport("steam_api64.dll", "SteamInternal_GameServer_Init_V2"), reinterpret_cast<void*>(&BlockedGameServerInit), nullptr) &&
        Patch(FindDelayImport("steam_api64.dll", "SteamInternal_FindOrCreateGameServerInterface"),
              reinterpret_cast<void*>(&BlockedGameServerInterface), nullptr);
    gImportsDone = lookups && writes && interfaces && servers;
    gImportReport = std::string("internet lookups ") + (lookups ? "blocked" : "NOT BLOCKED") + ", record writes " +
                    (writes ? "blocked" : "NOT BLOCKED") + ", Steam interfaces " + (interfaces ? "watched" : "NOT WATCHED") +
                    ", Steam game servers " + (servers ? "blocked" : "NOT BLOCKED");
    // The window (not a block: a copy that could take focus is a nuisance, not a leak, so it isn't in "complete").
    const bool quiet =
        Patch(FindImport("USER32.dll", "ShowWindow"), reinterpret_cast<void*>(&QuietShowWindow), reinterpret_cast<void**>(&gShowWindow)) &&
        Patch(FindImport("USER32.dll", "SetWindowPos"), reinterpret_cast<void*>(&QuietSetWindowPos), reinterpret_cast<void**>(&gSetWindowPos)) &&
        Patch(FindImport("USER32.dll", "SetWindowPlacement"), reinterpret_cast<void*>(&QuietSetWindowPlacement),
              reinterpret_cast<void**>(&gSetWindowPlacement)) &&
        Patch(FindImport("USER32.dll", "SetForegroundWindow"), reinterpret_cast<void*>(&QuietSetForegroundWindow),
              reinterpret_cast<void**>(&gSetForegroundWindow)) &&
        Patch(FindImport("USER32.dll", "SetActiveWindow"), reinterpret_cast<void*>(&QuietSetActiveWindow),
              reinterpret_cast<void**>(&gSetActiveWindow)) &&
        Patch(FindImport("USER32.dll", "SetFocus"), reinterpret_cast<void*>(&QuietSetFocus), reinterpret_cast<void**>(&gSetFocus));
    gWindowReport = quiet ? "the window stays behind and never takes focus" : "the window MAY TAKE FOCUS";
    if (!gOwnUserDir.empty()) gWindowReport += "; its own -userdir keeps its records: " + std::string(gOwnUserDir.begin(), gOwnUserDir.end());
}

namespace {
SRWLOCK gInstallLock = SRWLOCK_INIT;
bool InstallSteamNow();
bool InstallSteamLocked() {
    AcquireSRWLockExclusive(&gInstallLock);
    const bool done = InstallSteamNow();
    ReleaseSRWLockExclusive(&gInstallLock);
    return done;
}
}  // namespace

bool InstallSteam() { return InstallSteamLocked(); }

namespace {
bool PrepareGuard(HMODULE dll) {
    auto accessor = reinterpret_cast<void* (*)()>(GetProcAddress(dll, "SteamAPI_SteamUserStats_v013"));
    void* object = accessor ? accessor() : nullptr;
    if (!object) return false;
    void** table = *reinterpret_cast<void***>(object);
    std::string bytes;
    auto slot = [&](const char* method) -> void* {
        auto* wrapper = reinterpret_cast<const uint8_t*>(GetProcAddress(dll, method));
        const int offset = wrapper ? SlotOffset(wrapper, &bytes) : -1;
        return offset >= 0 && offset % 8 == 0 && offset <= 8 * 400 ? table[offset / 8] : nullptr;
    };
    gUploadFlat = slot("SteamAPI_ISteamUserStats_UploadLeaderboardScore");
    gAttachFlat = slot("SteamAPI_ISteamUserStats_AttachLeaderboardUGC");
    if (!gUploadFlat || !gAttachFlat || gUploadFlat == gAttachFlat) {
        hostlog::Error("upload guard: Steam's leaderboard methods not found (wrapper " + bytes + "); runs the host touched could reach leaderboards");
        gSteamDone = true;
        return true;
    }
    AcquireSRWLockExclusive(&gInterfaceLock);
    gGuardReady = gSteamDone = true;
    for (auto& gi : gGameInterfaces) GuardGameInterface(gi);
    ReleaseSRWLockExclusive(&gInterfaceLock);
    return true;
}

bool InstallSteamNow() {
    if (gSteamDone) return true;
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    if (!dll) return false;
    if (!gOn) {
        auto steamUser = reinterpret_cast<int32_t (*)()>(GetProcAddress(dll, "SteamAPI_GetHSteamUser"));
        return steamUser && steamUser() != 0 && PrepareGuard(dll);
    }
    // Not before the game has started Steam: asked earlier, an interface accessor can stay null for good (measured:
    // SteamUserStats_v013 never appeared when polled from launch).
    auto user = reinterpret_cast<int32_t (*)()>(GetProcAddress(dll, "SteamAPI_GetHSteamUser"));
    if (!user || user() == 0) return false;
    // All interfaces first: they exist once the game has started Steam.
    static DWORD lastReport = 0;
    for (const auto& b : gSteam) {
        auto accessor = reinterpret_cast<void* (*)()>(GetProcAddress(dll, b.accessor));
        if (!accessor || !accessor()) {
            if (GetTickCount() - lastReport > 10000) {
                lastReport = GetTickCount();
                hostlog::Info(std::string("sandbox: waiting for Steam's ") + b.accessor + (accessor ? " (not started yet)" : " (no such export)"));
            }
            return false;
        }
    }
    int installed = 0;
    for (auto& b : gSteam) {
        auto accessor = reinterpret_cast<void* (*)()>(GetProcAddress(dll, b.accessor));
        auto* wrapper = reinterpret_cast<const uint8_t*>(GetProcAddress(dll, b.method));
        void* object = accessor();
        std::string bytes;
        const int offset = wrapper ? SlotOffset(wrapper, &bytes) : -1;
        if (offset < 0 || offset % 8 != 0 || offset > 8 * 400) {
            hostlog::Error(std::string("sandbox: can't find ") + b.method + "'s slot (wrapper " + bytes + "); NOT BLOCKED");
            continue;
        }
        void** table = *reinterpret_cast<void***>(object);
        void* original = table[offset / 8];
        bool shared = false;            // two methods read as one slot: one of them was read wrong
        for (const auto& other : gSteam)
            shared = shared || (&other != &b && other.installed && std::strcmp(other.accessor, b.accessor) == 0 && other.stub == original);
        if (shared) {
            hostlog::Error(std::string("sandbox: ") + b.method + " read as another blocked method's slot (wrapper " + bytes + "); NOT BLOCKED");
            continue;
        }
        if (original != b.stub && Patch(table + offset / 8, b.stub, nullptr)) b.original = original;
        if (b.original) {
            b.installed = true;
            ++installed;
        }
    }
    for (const wchar_t* dllName : {L"steamclient64.dll", L"msquic.dll"})
        if (HMODULE m = GetModuleHandleW(dllName)) BlockNetworkIn(dllName, m);
    AcquireSRWLockExclusive(&gInterfaceLock);
    gSteamDone = true;
    for (auto& gi : gGameInterfaces) {                             // the ones the game asked for until now
        BlockGameInterface(gi);
        ChainGuard(gi);
    }
    ReleaseSRWLockExclusive(&gInterfaceLock);
    hostlog::Info("sandbox: " + gImportReport + "; Steam: " + std::to_string(installed) + " of " + std::to_string(kSteamBlocks) +
                  " upload/change methods blocked");
    hostlog::Info("sandbox: " + gWindowReport);
    hostlog::Info("sandbox: status " + Status());      // tools/test_instance.py waits for "status on, complete"
    return true;
}
}  // namespace

bool Complete() { return gOn && Status().rfind("on, complete", 0) == 0; }

std::string Status() {
    if (!gOn) return "off";
    int installed = 0;
    long blocked = 0;
    for (const auto& b : gSteam) {
        installed += b.installed ? 1 : 0;
        blocked += b.count;
    }
    // The game's own interfaces: each version it asked for, and its blocks. Without a leaderboard interface seen and
    // blocked, uploads can't be called safe.
    std::string game;
    bool gameComplete = gUnhookedRequests == 0, statsSeen = false;
    AcquireSRWLockShared(&gInterfaceLock);
    for (const auto& gi : gGameInterfaces) {
        if (!gi.blocked && !gi.missing) continue;      // a family with nothing to block (apps, screenshots, ...)
        game += (game.empty() ? "" : ", ") + gi.version + " " + std::to_string(gi.blocked) + (gi.missing ? " (" + std::to_string(gi.missing) + " MISSING)" : "");
        gameComplete = gameComplete && gi.missing == 0;
        statsSeen = statsSeen || (gi.version.rfind("STEAMUSERSTATS_INTERFACE_VERSION", 0) == 0 && gi.blocked > 0);
    }
    ReleaseSRWLockShared(&gInterfaceLock);
    // A networking DLL loaded after the blocks went in has its own imports, unblocked (msquic is loaded on demand).
    const bool lateQuic = GetModuleHandleW(L"msquic.dll") && gNetReport.find("msquic") == std::string::npos;
    const bool complete = gImportsDone && gSteamDone && installed == kSteamBlocks && gameComplete && statsSeen && gNetComplete &&
                          gFileComplete && !lateQuic;
    return std::string(complete ? "on, complete" : "on, INCOMPLETE") + " | " + gImportReport + " | Steam " + std::to_string(installed) + "/" +
           std::to_string(kSteamBlocks) + (gSteamDone ? "" : " (waiting for Steam)") + " | the game's interfaces: " +
           (game.empty() ? "NONE SEEN" : game) + (gUnhookedRequests ? " | SOME NOT SEEN" : "") + " | blocked so far: " +
           std::to_string(gBlockedLookups.load()) + " lookups, " + std::to_string(gBlockedWrites.load()) + " record writes, " +
           std::to_string(blocked + gPersonaCount + gBlockedServers) + " Steam calls, " + std::to_string(gBlockedConnections.load()) + " connections, " +
           std::to_string(gBlockedActivations.load()) + " window activations | network hooks: " + gNetReport + (lateQuic ? ", msquic.dll NOT BLOCKED" : "") +
           " | file hooks: " + gFileReport + " | " + gWindowReport;
}

std::string SelfTest() {
    if (!gOn) return "sandbox is off";
    std::string out;
    // 1. A name lookup through the game's own import, as its HTTP client makes them.
    if (gGetAddrInfoSlot) {
        auto lookup = reinterpret_cast<GetAddrInfo>(*gGetAddrInfoSlot);
        PADDRINFOA result = nullptr;
        const INT r = lookup("ballest-backend-prod.herokuapp.com", "443", nullptr, &result);
        out += r != 0 && !result ? "lookup blocked; " : "LOOKUP WENT THROUGH; ";
        if (result) freeaddrinfo(result);
    }
    // 2. A write into the save folder through the game's own import.
    if (gCreateFileSlot) {
        auto create = reinterpret_cast<CreateFileWFn>(*gCreateFileSlot);
        const std::wstring path = hostlog::DataDir() + L"\\..\\SaveGames\\sandbox_test.tmp";
        HANDLE h = create(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            out += "save write blocked; ";
        } else {
            CloseHandle(h);
            DeleteFileW(path.c_str());
            out += "SAVE WRITE WENT THROUGH; ";
        }
    }
    // 3. Steam, through the game's OWN leaderboard interface (the version it asked for, its table): a score for no
    // leaderboard (handle 0, which Steam rejects even if a block were missing) and storing the stats as they are.
    void* object = nullptr;
    AcquireSRWLockShared(&gInterfaceLock);
    for (const auto& gi : gGameInterfaces)
        if (gi.version.rfind("STEAMUSERSTATS_INTERFACE_VERSION", 0) == 0) object = gi.object;
    ReleaseSRWLockShared(&gInterfaceLock);
    int uploadSlot = -1, storeSlot = -1;
    if (object) {
        void** table = *reinterpret_cast<void***>(object);
        for (int i = 0; i < 400 && Readable(table + i) && Executable(table[i]); ++i) {
            if (table[i] == gSteam[0].stub || table[i] == reinterpret_cast<void*>(&GuardedUpload)) uploadSlot = i;
            if (table[i] == gSteam[8].stub) storeSlot = i;
        }
    }
    if (uploadSlot >= 0 && storeSlot >= 0) {
        void** table = *reinterpret_cast<void***>(object);
        const long before = gSteam[0].count + gSteam[8].count;
        using Upload = uint64_t (*)(void*, uint64_t, int, int32_t, const int32_t*, int);
        using Store = bool (*)(void*);
        const uint64_t call = reinterpret_cast<Upload>(table[uploadSlot])(object, 0, 1, 1, nullptr, 0);
        const bool stored = reinterpret_cast<Store>(table[storeSlot])(object);
        out += gSteam[0].count + gSteam[8].count == before + 2 && call == 0 && !stored
                   ? "Steam uploads blocked in the game's own interface"
                   : "STEAM WENT THROUGH";
    } else {
        out += object ? "STEAM NOT BLOCKED in the game's own interface" : "the game's leaderboard interface not seen yet";
    }
    return out;
}

}  // namespace sandbox
