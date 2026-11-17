#include <iostream>
#include <string>

#include "../../src/host/codec.hpp"

namespace {
int failures = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::cout << "FAIL " << what << '\n';
}
}  // namespace

int main() {
    Check(codec::Digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256");
    Check(codec::Pack("").empty(), "empty input refused");
    Check(codec::Unpack("").empty(), "empty packed text refused");
    Check(codec::Unpack("A").empty(), "invalid base64 length refused");
    Check(codec::Unpack("not base64!").empty(), "invalid base64 character refused");

    std::string text;
    for (int i = 0; i < 400; ++i) text += "round map snapshot | player placements | replay data\n";
    const std::string packed = codec::Pack(text);
    Check(!packed.empty(), "ordinary text packed");
    Check(packed.find('=') == std::string::npos, "output is unpadded base64");
    Check(codec::Unpack(packed) == text, "ordinary text round trip");
    Check(codec::Pack(text) == packed, "packing is deterministic");

    std::string binary;
    for (int repeat = 0; repeat < 32; ++repeat)
        for (int byte = 0; byte < 256; ++byte) binary += static_cast<char>(byte);
    Check(codec::Unpack(codec::Pack(binary)) == binary, "binary string round trip");

    const std::string maximum(codec::kMaxInput, 'x');
    Check(codec::Unpack(codec::Pack(maximum)) == maximum, "maximum-size round trip");
    const std::string oversized(codec::kMaxInput + 1, 'x');
    Check(codec::Pack(oversized).empty(), "oversized pack refused");
    Check(codec::Digest(oversized).empty(), "oversized digest refused");

    std::string damaged = packed;
    if (damaged.size() > 12) damaged[damaged.size() / 2] = damaged[damaged.size() / 2] == 'A' ? 'B' : 'A';
    Check(codec::Unpack(damaged) != text, "damaged packed text not accepted as the original");

    std::cout << "codec_test: " << (failures ? "failed" : "ok") << " (" << failures << " failure(s))\n";
    return failures ? 1 : 0;
}
