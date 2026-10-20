// Files written whole and safely: to "<file>.part" first, then moved over the file, so a crash mid-write never leaves
// it half written (a broken icon kept for good, stored settings lost). Shared by the registry's downloads and icons,
// plugins' Storage, and the cosmetics' loadouts and settings.
#pragma once
#include <string>

namespace files {

// False if it couldn't be written or moved into place; the file is then left as it was. The move is written through
// (on disk when it returns) and tried a few times over a short moment: something holding the file open without
// FILE_SHARE_DELETE (antivirus, the indexer) makes it fail for a while.
bool WriteWhole(const std::wstring& path, const std::string& data);

}  // namespace files
