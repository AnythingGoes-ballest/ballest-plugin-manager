// Generated: pointers to the real version.dll exports, filled by ResolveVersionExports().
#include <windows.h>
extern "C" {
void* real_GetFileVersionInfoA = nullptr;
void* real_GetFileVersionInfoByHandle = nullptr;
void* real_GetFileVersionInfoExA = nullptr;
void* real_GetFileVersionInfoExW = nullptr;
void* real_GetFileVersionInfoSizeA = nullptr;
void* real_GetFileVersionInfoSizeExA = nullptr;
void* real_GetFileVersionInfoSizeExW = nullptr;
void* real_GetFileVersionInfoSizeW = nullptr;
void* real_GetFileVersionInfoW = nullptr;
void* real_VerFindFileA = nullptr;
void* real_VerFindFileW = nullptr;
void* real_VerInstallFileA = nullptr;
void* real_VerInstallFileW = nullptr;
void* real_VerLanguageNameA = nullptr;
void* real_VerLanguageNameW = nullptr;
void* real_VerQueryValueA = nullptr;
void* real_VerQueryValueW = nullptr;
}
static const char* kNames[] = {"GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW", "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW", "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW", "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW"};
static void** kSlots[] = {&real_GetFileVersionInfoA, &real_GetFileVersionInfoByHandle, &real_GetFileVersionInfoExA, &real_GetFileVersionInfoExW, &real_GetFileVersionInfoSizeA, &real_GetFileVersionInfoSizeExA, &real_GetFileVersionInfoSizeExW, &real_GetFileVersionInfoSizeW, &real_GetFileVersionInfoW, &real_VerFindFileA, &real_VerFindFileW, &real_VerInstallFileA, &real_VerInstallFileW, &real_VerLanguageNameA, &real_VerLanguageNameW, &real_VerQueryValueA, &real_VerQueryValueW};

bool ResolveVersionExports() {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\version.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real) return false;
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) *kSlots[i] = (void*)GetProcAddress(real, kNames[i]);
    return true;
}
