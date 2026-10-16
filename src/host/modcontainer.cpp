// The gate on plugin containers. Reads a mod's IoStore .utoc to (a) list every package it ships
// and (b) prove none of them can shadow a game or engine package: every package's path must be under
// /Game/Mods/<plugin id>/, and every package id in the container must be the CityHash64 of such a path. This reads
// only the .utoc (directory index for the paths, the type-1 chunk ids for the package ids) -- no .ucas, no Oodle.
// See docs/custom-assets-recipe.md and assets.hpp.
#include "modcontainer.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace modcontainer {
namespace {

// --- CityHash64 (the UE variant; matches FPackageId / FIoChunkId hashing; verified against the game's containers) ---
constexpr uint64_t k0 = 0xc3a5c85c97cb3127ULL, k1 = 0xb492b66fbe98f273ULL, k2 = 0x9ae16a3b2f90404fULL;
uint64_t Fetch64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
uint32_t Fetch32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t Rotate(uint64_t v, int s) { return s == 0 ? v : (v >> s) | (v << (64 - s)); }
uint64_t ShiftMix(uint64_t v) { return v ^ (v >> 47); }
uint64_t Bswap64(uint64_t v) {
    return ((v & 0xffULL) << 56) | ((v & 0xff00ULL) << 40) | ((v & 0xff0000ULL) << 24) | ((v & 0xff000000ULL) << 8) |
           ((v >> 8) & 0xff000000ULL) | ((v >> 24) & 0xff0000ULL) | ((v >> 40) & 0xff00ULL) | ((v >> 56) & 0xffULL);
}
uint64_t Hash16(uint64_t u, uint64_t v, uint64_t mul = 0x9ddfea08eb382d69ULL) {
    uint64_t a = (u ^ v) * mul; a ^= a >> 47;
    uint64_t b = (v ^ a) * mul; b ^= b >> 47; b *= mul; return b;
}
uint64_t Hash0to16(const uint8_t* s, size_t n) {
    if (n >= 8) { uint64_t mul = k2 + n * 2, a = Fetch64(s) + k2, b = Fetch64(s + n - 8);
        uint64_t c = Rotate(b, 37) * mul + a, d = (Rotate(a, 25) + b) * mul; return Hash16(c, d, mul); }
    if (n >= 4) { uint64_t mul = k2 + n * 2, a = Fetch32(s); return Hash16(n + (a << 3), Fetch32(s + n - 4), mul); }
    if (n > 0) { uint8_t a = s[0], b = s[n >> 1], c = s[n - 1];
        uint32_t y = (uint32_t)a + ((uint32_t)b << 8), z = (uint32_t)n + ((uint32_t)c << 2);
        return ShiftMix(y * k2 ^ z * k0) * k2; }
    return k2;
}
uint64_t Hash17to32(const uint8_t* s, size_t n) {
    uint64_t mul = k2 + n * 2, a = Fetch64(s) * k1, b = Fetch64(s + 8), c = Fetch64(s + n - 8) * mul, d = Fetch64(s + n - 16) * k2;
    return Hash16(Rotate(a + b, 43) + Rotate(c, 30) + d, a + Rotate(b + k2, 18) + c, mul);
}
uint64_t Hash33to64(const uint8_t* s, size_t n) {
    uint64_t mul = k2 + n * 2, a = Fetch64(s) * k2, b = Fetch64(s + 8), c = Fetch64(s + n - 24), d = Fetch64(s + n - 32);
    uint64_t e = Fetch64(s + 16) * k2, f = Fetch64(s + 24) * 9, g = Fetch64(s + n - 8), h = Fetch64(s + n - 16) * mul;
    uint64_t u = Rotate(a + g, 43) + (Rotate(b, 30) + c) * 9, v = ((a + g) ^ d) + f + 1;
    uint64_t w = Bswap64((u + v) * mul) + h, x = Rotate(e + f, 42) + c;
    uint64_t y = (Bswap64((v + w) * mul) + g) * mul, z = e + f + c;
    a = Bswap64((x + z) * mul + y) + b; b = ShiftMix((z + a) * mul + d + h) * mul; return b + x;
}
void WeakHash(const uint8_t* s, uint64_t a, uint64_t b, uint64_t& o0, uint64_t& o1) {
    uint64_t w = Fetch64(s), x = Fetch64(s + 8), y = Fetch64(s + 16), z = Fetch64(s + 24);
    a += w; b = Rotate(b + a + z, 21); uint64_t c = a; a += x; a += y; b += Rotate(a, 44); o0 = a + z; o1 = b + c;
}
uint64_t CityHash64(const uint8_t* s, size_t n) {
    if (n <= 32) return n <= 16 ? Hash0to16(s, n) : Hash17to32(s, n);
    if (n <= 64) return Hash33to64(s, n);
    uint64_t x = Fetch64(s + n - 40), y = Fetch64(s + n - 16) + Fetch64(s + n - 56), z = Hash16(Fetch64(s + n - 48) + n, Fetch64(s + n - 24));
    uint64_t v0, v1, w0, w1;
    WeakHash(s + n - 64, n, z, v0, v1); WeakHash(s + n - 32, y + k1, x, w0, w1);
    x = x * k1 + Fetch64(s);
    size_t len = (n - 1) & ~(size_t)63; const uint8_t* p = s;
    do {
        x = Rotate(x + y + v0 + Fetch64(p + 8), 37) * k1;
        y = Rotate(y + v1 + Fetch64(p + 48), 42) * k1;
        x ^= w1; y += v0 + Fetch64(p + 40); z = Rotate(z + w0, 33) * k1;
        WeakHash(p, v1 * k1, x + w0, v0, v1); WeakHash(p + 32, z + w1, y + Fetch64(p + 16), w0, w1);
        uint64_t t = z; z = x; x = t;
        p += 64; len -= 64;
    } while (len != 0);
    return Hash16(Hash16(v0, w0) + ShiftMix(y) * k1 + z, Hash16(v1, w1) + x);
}

uint64_t PackageIdFromName(const std::string& name) {  // CityHash64 of the lower-cased UTF-16LE path
    std::vector<uint8_t> u16; u16.reserve(name.size() * 2);
    for (char c : name) { char lc = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; u16.push_back((uint8_t)lc); u16.push_back(0); }
    return CityHash64(u16.data(), u16.size());
}

// --- a tiny reader over the .utoc bytes ---
struct Reader {
    const uint8_t* d; size_t n, p = 0;
    uint32_t U32() { uint32_t v = 0; if (p + 4 <= n) std::memcpy(&v, d + p, 4); p += 4; return v; }
    int32_t I32() { return (int32_t)U32(); }
    std::string FString() {
        int32_t len = I32(); std::string s;
        if (len >= 0) { for (int i = 0; i + 1 < len; ++i) s.push_back((char)d[p + i]); p += len; }
        else { int cnt = -len; for (int i = 0; i + 1 < cnt; ++i) s.push_back((char)d[p + i * 2]); p += 2 * cnt; }
        return s;
    }
};

}  // namespace

bool Verify(const std::wstring& utocPath, const std::string& pluginId, std::string& error, std::string* shaderLibrary) {
    HANDLE fh = CreateFileW(utocPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (fh == INVALID_HANDLE_VALUE) { error = "cannot open .utoc"; return false; }
    LARGE_INTEGER sz{}; GetFileSizeEx(fh, &sz);
    std::vector<uint8_t> b((size_t)sz.QuadPart);
    DWORD got = 0;
    if (!b.empty()) ReadFile(fh, b.data(), (DWORD)b.size(), &got, nullptr);
    CloseHandle(fh);
    if (got != b.size()) { error = "short read on .utoc"; return false; }
    if (b.size() < 144 || std::memcmp(b.data(), "-==--==--==--==-", 16) != 0) { error = "not an IoStore TOC"; return false; }
    auto rd32 = [&](size_t o) { uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; };
    // FIoStoreTocHeader: TocHeaderSize@20, TocEntryCount@24, TocCompressedBlockEntryCount@28,
    // TocCompressedBlockEntrySize@32, CompressionMethodNameCount@36, CompressionMethodNameLength@40,
    // CompressionBlockSize@44, DirectoryIndexSize@48, PartitionCount@52, ContainerId@56, Guid@64, Flags@80,
    // PerfectHashSeedsCount@84, PartitionSize@88, ChunksWithoutPerfectHashCount@96.
    uint32_t hsz = rd32(20), entryCount = rd32(24), blockCount = rd32(28);
    uint32_t cmCount = rd32(36), cmLen = rd32(40), dirSize = rd32(48);
    uint8_t flags = b[80];
    if (flags & 0x2) { error = "container is encrypted"; return false; }
    if (!(flags & 0x8) || dirSize == 0) { error = "container has no directory index"; return false; }

    // Section offsets after the header: chunk ids (12 B each), offset/length (10 B), perfect-hash seeds, blocks (12 B),
    // compression method names, signatures (none: unsigned), directory index, meta.
    size_t p = hsz;
    const uint8_t* ids = b.data() + p; p += 12ull * entryCount;
    p += 10ull * entryCount;                                  // offset/length
    p += 4ull * rd32(84);                                     // perfect-hash seeds (TocChunkPerfectHashSeedsCount)
    p += 4ull * rd32(96);                                     // chunks without perfect hash
    p += 12ull * blockCount;                                  // compression blocks
    p += (size_t)cmCount * cmLen;                             // compression method names
    // unsigned -> no signature block. Directory index follows.
    const uint8_t* dir = b.data() + p;
    if (p + dirSize > b.size()) { error = "directory index out of range"; return false; }

    // Parse the directory index into full file paths.
    Reader r{dir, dirSize};
    std::string mount = r.FString();
    struct DEntry { uint32_t name, firstChild, nextSibling, firstFile; };
    struct FEntry { uint32_t name, nextFile, userData; };
    uint32_t nd = r.U32(); std::vector<DEntry> dirs(nd);
    for (auto& e : dirs) { e.name = r.U32(); e.firstChild = r.U32(); e.nextSibling = r.U32(); e.firstFile = r.U32(); }
    uint32_t nf = r.U32(); std::vector<FEntry> files(nf);
    for (auto& e : files) { e.name = r.U32(); e.nextFile = r.U32(); e.userData = r.U32(); }
    uint32_t ns = r.U32(); std::vector<std::string> strs(ns);
    for (auto& s : strs) s = r.FString();
    auto nameOf = [&](uint32_t i) -> std::string { return i == 0xFFFFFFFFu || i >= ns ? std::string() : strs[i]; };

    // The allowed package path prefix and the mod's own shader-library file name prefix.
    const std::string prefix = "Mods/" + pluginId + "/";
    const std::string shaderPrefix = "ShaderArchive-";
    std::set<uint64_t> allowedPackageIds;
    bool bad = false; std::string badPath;
    // Walk the tree (iterative to avoid recursion into a crafted deep index).
    struct Frame { uint32_t dirIdx; std::string path; };
    std::vector<Frame> stack{{0, ""}};
    size_t guard = 0;
    while (!stack.empty()) {
        if (++guard > (size_t)nd + 4) { error = "directory index cycle"; return false; }
        Frame fr = stack.back(); stack.pop_back();
        if (fr.dirIdx >= nd) continue;
        const DEntry& de = dirs[fr.dirIdx];
        std::string here = fr.path + (de.name == 0xFFFFFFFFu ? "" : nameOf(de.name) + "/");
        for (uint32_t fi = de.firstFile; fi != 0xFFFFFFFFu && fi < nf; fi = files[fi].nextFile) {
            std::string file = here + nameOf(files[fi].name);
            bool isShader = file.rfind(shaderPrefix, 0) == 0 && file.find('/') == std::string::npos &&
                            file.size() > 16 && file.compare(file.size() - 16, 16, ".ushaderbytecode") == 0;
            if (isShader) {
                // "ShaderArchive-<library>-<format>-<platform>.ushaderbytecode": the library must be the plugin's own,
                // "<id>" or "<id>_<anything>", so it can never collide with the game's "Ballest" or "Global".
                std::string rest = file.substr(shaderPrefix.size());
                const size_t dash = rest.find("-PCD3D_");
                const std::string lib = dash == std::string::npos ? std::string() : rest.substr(0, dash);
                if (lib != pluginId && lib.rfind(pluginId + "_", 0) != 0) {
                    error = "shader library '" + lib + "' is not the plugin's own (" + pluginId + " or " + pluginId + "_*)";
                    return false;
                }
                if (shaderLibrary) {
                    if (!shaderLibrary->empty() && *shaderLibrary != lib) { error = "more than one shader library"; return false; }
                    *shaderLibrary = lib;
                }
            }
            auto endsWith = [&](const char* tail) {
                const size_t n = std::strlen(tail);
                return file.size() > n && file.compare(file.size() - n, n, tail) == 0;
            };
            const bool isModPkg = file.rfind(prefix, 0) == 0 && (endsWith(".uasset") || endsWith(".umap"));
            if (isModPkg) {
                std::string pkg = file.substr(0, file.rfind('.'));
                allowedPackageIds.insert(PackageIdFromName("/Game/" + pkg));
            } else if (!isShader) {
                // Allow only the mod's packages and its shader library; refuse anything else (incl. loose game paths).
                if (!(file.find('/') == std::string::npos && (file.size() > 5 && file.compare(file.size() - 5, 5, ".json") == 0))) {
                    bad = true; badPath = file;
                }
            }
        }
        for (uint32_t ci = de.firstChild; ci != 0xFFFFFFFFu && ci < nd; ci = dirs[ci].nextSibling)
            stack.push_back({ci, here});
    }
    if (bad) { error = "container ships a file outside Mods/" + pluginId + "/: " + badPath; return false; }

    // Every package the container actually serves (type-1 chunk ids) must be one of the allowed /Game/Mods/<id> ids.
    for (uint32_t i = 0; i < entryCount; ++i) {
        const uint8_t* cid = ids + 12ull * i;
        if (cid[11] != 1) continue;                          // 1 = ExportBundleData (a package)
        uint64_t pkgId; std::memcpy(&pkgId, cid, 8);
        if (!allowedPackageIds.count(pkgId)) {
            error = "container serves a package whose id is not under /Game/Mods/" + pluginId + "/ (possible game-asset shadow)";
            return false;
        }
    }
    (void)mount;
    return true;
}

}  // namespace modcontainer
