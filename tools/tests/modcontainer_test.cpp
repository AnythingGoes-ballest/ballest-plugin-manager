// modcontainer::Verify (the gate on plugin-shipped cooked containers), offline. Built and run by tools/tests/run.sh.
// Needs a real mod container: set MOD_UTOC to a "<mod>_bNNNN-Windows.utoc" made by tools/make_mod_container.py and
// MOD_NAME to its mod name. Skipped when they're missing (a container is a build artefact, never committed).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../src/host/modcontainer.hpp"

int main() {
    const char* utoc = std::getenv("MOD_UTOC");
    const char* mod = std::getenv("MOD_NAME");
    if (!utoc || !mod) {
        std::puts("modcontainer_test skipped (set MOD_UTOC and MOD_NAME)");
        return 0;
    }
    const std::string path(utoc);
    const std::wstring wpath(path.begin(), path.end());
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failures; std::printf("FAIL %s\n", what.c_str()); }
    };
    std::string err, lib;
    check(modcontainer::Verify(wpath, mod, err, &lib), std::string("accepted under its own mod name: ") + err);
    check(lib.rfind(std::string(mod), 0) == 0, "shader library named after the mod: " + lib);
    std::string err2;
    check(!modcontainer::Verify(wpath, "someone_else", err2), "refused under another mod name");
    check(err2.find("outside Mods/someone_else/") != std::string::npos || err2.find("not the plugin's own") != std::string::npos,
          "refusal says why: " + err2);
    std::string err3;
    check(!modcontainer::Verify(L"C:\\no\\such\\file.utoc", mod, err3), "missing file refused");
    std::printf("modcontainer_test: %s (%d failure(s))\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
