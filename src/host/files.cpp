#include "files.hpp"

#include <windows.h>

namespace files {

bool WriteWhole(const std::wstring& path, const std::string& data) {
    const std::wstring part = path + L".part";
    HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    CloseHandle(h);
    if (ok)
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
            Sleep(15);
        }
    DeleteFileW(part.c_str());
    return false;
}

}  // namespace files
