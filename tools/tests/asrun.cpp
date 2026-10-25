// Runs a plugin's script code without the game: the host's engine and API (as ascheck), the plugin and its
// dependencies built as the host builds them, plus a test script added to the plugin's module, whose
// `void Test()` is called. For code that only computes (fitting, geometry): anything that reaches into the game
// (Editor::, Camera::, UI::) must not be called from a test.
//   asrun <plugin folder> <test.as>
// The test script can also use:
//   void Print(const string &in)        a line on stdout
//   string TestRead(const string &in)   a file from the plugin's folder (as Plugins::ReadFile would give it)
//   void Fail(const string &in)         a failed check: printed, and the exit code is 1
// LOCALAPPDATA is pointed at a scratch folder first, so nothing can be written into the player's data.
#include <windows.h>

#include <angelscript.h>
#include <scriptbuilder/scriptbuilder.h>
#include <scriptstdstring/scriptstdstring.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/host/api.hpp"

static int gErrors = 0, gFailures = 0;
static std::string gPluginDir;

static void Message(const asSMessageInfo* m, void*) {
    if (m->type == asMSGTYPE_INFORMATION) return;
    if (m->type == asMSGTYPE_ERROR) ++gErrors;
    std::printf("%s (%d, %d): %s\n", m->section, m->row, m->col, m->message);
}

static std::string Read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    std::string out;
    for (char c : s)
        if (c != '\r') out += c;
    return out;
}

static std::vector<std::string> List(const std::string& toml, const std::string& key) {
    std::vector<std::string> out;
    const size_t at = toml.find("\n" + key);
    if (at == std::string::npos) return out;
    const size_t open = toml.find('[', at), close = toml.find(']', open);
    if (open == std::string::npos || close == std::string::npos) return out;
    const std::string body = toml.substr(open + 1, close - open - 1);
    for (size_t q = body.find('"'); q != std::string::npos; q = body.find('"', q + 1)) {
        const size_t end = body.find('"', q + 1);
        if (end == std::string::npos) break;
        out.push_back(body.substr(q + 1, end - q - 1));
        q = end;
    }
    return out;
}

static std::string Folder(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? "." : path.substr(0, cut);
}
static std::string Name(const std::string& path) {
    std::string p = path;
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? p : p.substr(cut + 1);
}

static void Print(const std::string& s) { std::printf("%s\n", s.c_str()); }
static std::string TestRead(const std::string& file) { return Read(gPluginDir + "/" + file); }
static void Fail(const std::string& s) {
    ++gFailures;
    std::printf("FAIL: %s\n", s.c_str());
}

static bool Build(asIScriptEngine* e, const std::string& dir, const std::string& id, const std::string& extra) {
    const std::string toml = "\n" + Read(dir + "/info.toml");
    for (const auto& dep : List(toml, "dependencies")) {
        bool found = false;
        for (const std::string& candidate : {Folder(dir) + "/" + dep, Folder(dir) + "/ballest-" + dep})
            if (!Read(candidate + "/info.toml").empty()) {
                found = Build(e, candidate, dep, "");
                break;
            }
        if (!found) return std::printf("%s: dependency %s not found\n", id.c_str(), dep.c_str()), false;
    }
    CScriptBuilder builder;
    if (builder.StartNewModule(e, id.c_str()) < 0) return false;
    for (const auto& file : List(toml, "files")) {
        const std::string code = Read(dir + "/" + file);
        builder.AddSectionFromMemory((id + "/" + file).c_str(), code.data(), static_cast<unsigned>(code.size()));
    }
    if (!extra.empty()) {
        const std::string code = Read(extra);
        if (code.empty()) return std::printf("no test script %s\n", extra.c_str()), false;
        builder.AddSectionFromMemory(extra.c_str(), code.data(), static_cast<unsigned>(code.size()));
    }
    if (builder.BuildModule() < 0) return std::printf("%s: does not compile\n", id.c_str()), false;
    asIScriptModule* m = builder.GetModule();
    if (m->GetImportedFunctionCount() > 0 && m->BindAllImportedFunctions() < 0)
        return std::printf("%s: an import was not found\n", id.c_str()), false;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) return std::printf("usage: asrun <plugin folder> <test.as>\n"), 2;
    // nothing the host code does here may touch the player's data: its folder is a scratch one for this run
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring scratch = std::wstring(temp) + L"asrun-localappdata";
    CreateDirectoryW(scratch.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str());

    asIScriptEngine* e = asCreateScriptEngine();
    e->SetMessageCallback(asFUNCTION(Message), nullptr, asCALL_CDECL);
    api::Register(e);
    e->SetDefaultNamespace("");
    e->RegisterGlobalFunction("void Print(const string &in)", asFUNCTION(Print), asCALL_CDECL);
    e->RegisterGlobalFunction("string TestRead(const string &in)", asFUNCTION(TestRead), asCALL_CDECL);
    e->RegisterGlobalFunction("void Fail(const string &in)", asFUNCTION(Fail), asCALL_CDECL);
    if (gErrors) return std::printf("the API did not register\n"), 1;

    gPluginDir = argv[1];
    std::string id = Name(argv[1]);
    if (id.rfind("ballest-", 0) == 0) id = id.substr(8);
    if (!Build(e, argv[1], id, argv[2])) return 1;
    asIScriptModule* m = e->GetModule(id.c_str());
    asIScriptFunction* test = m->GetFunctionByDecl("void Test()");
    if (!test) return std::printf("no void Test()\n"), 1;
    asIScriptContext* ctx = e->CreateContext();
    ctx->Prepare(test);
    const int r = ctx->Execute();
    if (r == asEXECUTION_EXCEPTION) {
        const asIScriptFunction* fn = ctx->GetExceptionFunction();
        std::printf("EXCEPTION: %s in %s line %d\n", ctx->GetExceptionString(), fn ? fn->GetDeclaration() : "?",
                    ctx->GetExceptionLineNumber());
        return 1;
    }
    if (r != asEXECUTION_FINISHED) return std::printf("execution ended: %d\n", r), 1;
    std::printf("%s\n", gFailures ? "FAILED" : "all checks passed");
    return gFailures ? 1 : 0;
}
