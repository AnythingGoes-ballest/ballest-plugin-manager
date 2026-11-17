#include "codec.hpp"

#include <windows.h>

#include <cstring>

#include "net.hpp"

namespace codec {
namespace {

using CreateCodec = BOOL(WINAPI*)(DWORD, void*, void**);
using CloseCodec = BOOL(WINAPI*)(void*);
using Code = BOOL(WINAPI*)(void*, void*, SIZE_T, void*, SIZE_T, SIZE_T*);

struct Codec {
    CreateCodec create = nullptr;
    CloseCodec close = nullptr;
    Code code = nullptr;

    explicit Codec(bool compress) {
        // This is a fixed Windows component, never a DLL beside the game or on PATH.
        static HMODULE cabinet = LoadLibraryExW(L"cabinet.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!cabinet) return;
        create = reinterpret_cast<CreateCodec>(GetProcAddress(cabinet, compress ? "CreateCompressor" : "CreateDecompressor"));
        close = reinterpret_cast<CloseCodec>(GetProcAddress(cabinet, compress ? "CloseCompressor" : "CloseDecompressor"));
        code = reinterpret_cast<Code>(GetProcAddress(cabinet, compress ? "Compress" : "Decompress"));
    }
};

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Encode(const std::string& bytes) {
    std::string out;
    out.reserve((bytes.size() * 4 + 2) / 3);
    unsigned buffer = 0;
    int bits = 0;
    for (const unsigned char c : bytes) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += kBase64[(buffer >> bits) & 63];
        }
    }
    if (bits) out += kBase64[(buffer << (6 - bits)) & 63];
    return out;
}

std::string Decode(const std::string& text) {
    if (text.size() > kMaxInput * 2 || text.size() % 4 == 1) return "";
    std::string out;
    out.reserve(text.size() * 3 / 4);
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const char* found = std::strchr(kBase64, c);
        if (!found) return "";
        buffer = (buffer << 6) | static_cast<unsigned>(found - kBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 255);
        }
    }
    return out;
}

}  // namespace

std::string Digest(const std::string& text) { return text.size() <= kMaxInput ? net::Sha256(text) : ""; }

std::string Pack(const std::string& text) {
    if (text.empty() || text.size() > kMaxInput) return "";
    Codec codec(true);
    void* handle = nullptr;
    if (!codec.create || !codec.close || !codec.code || !codec.create(4, nullptr, &handle)) return "";  // XPRESS_HUFF

    SIZE_T count = 0;
    codec.code(handle, const_cast<char*>(text.data()), text.size(), nullptr, 0, &count);
    if (!count || count > kMaxInput * 2) {
        codec.close(handle);
        return "";
    }
    std::string bytes(count + 4, '\0');
    const uint32_t length = static_cast<uint32_t>(text.size());
    std::memcpy(bytes.data(), &length, sizeof length);
    const bool ok = codec.code(handle, const_cast<char*>(text.data()), text.size(), bytes.data() + 4, count, &count) != 0;
    codec.close(handle);
    if (!ok) return "";
    bytes.resize(count + 4);
    return Encode(bytes);
}

std::string Unpack(const std::string& packed) {
    std::string bytes = Decode(packed);
    uint32_t length = 0;
    if (bytes.size() < 5) return "";
    std::memcpy(&length, bytes.data(), sizeof length);
    if (!length || length > kMaxInput) return "";

    Codec codec(false);
    void* handle = nullptr;
    if (!codec.create || !codec.close || !codec.code || !codec.create(4, nullptr, &handle)) return "";  // XPRESS_HUFF
    std::string out(length, '\0');
    SIZE_T written = 0;
    const bool ok = codec.code(handle, bytes.data() + 4, bytes.size() - 4, out.data(), length, &written) != 0;
    codec.close(handle);
    return ok && written == length ? out : "";
}

}  // namespace codec
