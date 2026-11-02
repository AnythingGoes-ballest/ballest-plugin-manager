#include "screen.hpp"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "engine.hpp"
#include "game.hpp"
#include "log.hpp"

namespace screen {
namespace {

constexpr UINT kThumbW = 480, kThumbH = 300;
constexpr double kTimeout = 5;              // seconds for the game to write its screenshot

struct Shot {
    std::string state = "idle";
    std::wstring target;
    FILETIME after{};
    double deadline = 0;
    bool waiting = false;                   // for the game's screenshot (game thread)
};

std::mutex gLock;                           // guards the states (the thumbnail is made on a worker thread)
std::map<int, std::shared_ptr<Shot>> gCaptures;

// The game's Saved folder: a test copy's -userdir, else the player's.
std::wstring SavedDir() {
    const std::wstring line = GetCommandLineW();
    const size_t at = line.find(L"-userdir=");
    if (at != std::wstring::npos) {
        size_t start = at + 9, end;
        if (start < line.size() && line[start] == L'"') end = line.find(L'"', ++start);
        else end = line.find(L' ', start);
        std::wstring dir = line.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'"')) dir.pop_back();
        return dir + L"\\Saved";
    }
    wchar_t local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return std::wstring(local) + L"\\Ballest\\Saved";
}

template <class T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// The middle of `from` at 16:10, scaled to kThumbW x kThumbH, written to `to` as a PNG (no alpha: the game's
// screenshots carry none worth keeping).
bool MakeThumb(const std::wstring& from, const std::wstring& to, std::string& error) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapClipper* clipper = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* out = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) error = "no image codecs";
    else if (FAILED(factory->CreateDecoderFromFilename(from.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
             FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || w < 16 || h < 10)
        error = "the game's screenshot couldn't be read";
    else {
        UINT cw = w, ch = w * 10 / 16;
        if (ch > h) {
            ch = h;
            cw = h * 16 / 10;
        }
        const WICRect middle{static_cast<INT>((w - cw) / 2), static_cast<INT>((h - ch) / 2), static_cast<INT>(cw), static_cast<INT>(ch)};
        if (FAILED(factory->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(frame, &middle)) ||
            FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(clipper, kThumbW, kThumbH, WICBitmapInterpolationModeFant)) ||
            FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(scaler, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            error = "the picture couldn't be cut to size";
        else if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(to.c_str(), GENERIC_WRITE)) ||
                 FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
                 FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&out, &props)) ||
                 FAILED(out->Initialize(props)) || FAILED(out->SetSize(kThumbW, kThumbH)) || FAILED(out->SetPixelFormat(&format)) ||
                 FAILED(out->WriteSource(converter, nullptr)) || FAILED(out->Commit()) || FAILED(encoder->Commit()))
            error = "the picture couldn't be written";
        else
            ok = true;
    }
    Release(props);
    Release(out);
    Release(encoder);
    Release(stream);
    Release(converter);
    Release(scaler);
    Release(clipper);
    Release(frame);
    Release(decoder);
    Release(factory);
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

}  // namespace

bool Capture(int owner, const std::string& pluginId, const std::string& name, std::string* why) {
    auto refuse = [&](const std::string& reason) {
        if (why) *why = reason;
        hostlog::Write("warn", pluginId, "Screen::Capture refused: " + reason);
        return false;
    };
    if (owner < 0) return refuse("not called from a plugin");
    const bool plain = !name.empty() && name.size() <= 96 && name[0] != '.' && std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
    });
    if (!plain) return refuse("'" + name.substr(0, 96) + "' isn't a plain name (letters, digits, -_.)");
    std::lock_guard<std::mutex> hold(gLock);
    for (const auto& [id, c] : gCaptures)
        if (c->state == "pending") return refuse("another capture is under way");
    std::wstring dir = hostlog::DataDir() + L"\\cache";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\" + std::wstring(pluginId.begin(), pluginId.end());
    CreateDirectoryW(dir.c_str(), nullptr);
    auto c = std::make_shared<Shot>();
    c->target = dir + L"\\" + std::wstring(name.begin(), name.end()) + L".png";
    GetSystemTimeAsFileTime(&c->after);
    c->deadline = game::Seconds() + kTimeout;
    // the game's own screenshot of the view alone: no "showui", so neither the game's UI nor the plugins' windows
    const std::wstring command = L"shot";
    eng::Obj library = eng::FindCdo("KismetSystemLibrary");
    eng::Params p(eng::FunctionOn(library, "ExecuteConsoleCommand"));
    p.Set("WorldContextObject", game::PlayerController());
    p.Set("Command", eng::FString{command.c_str(), static_cast<int32_t>(command.size() + 1), static_cast<int32_t>(command.size() + 1)});
    if (!library || !eng::Invoke(library, p)) return refuse("the game's console isn't there");
    c->state = "pending";
    c->waiting = true;
    gCaptures[owner] = c;
    return true;
}

std::string State(int owner) {
    std::lock_guard<std::mutex> hold(gLock);
    auto it = gCaptures.find(owner);
    return it == gCaptures.end() ? "idle" : it->second->state;
}

void Frame() {
    std::vector<std::shared_ptr<Shot>> waiting;
    {
        std::lock_guard<std::mutex> hold(gLock);
        for (const auto& [id, c] : gCaptures)
            if (c->waiting) waiting.push_back(c);
    }
    if (waiting.empty()) return;
    const std::wstring folder = SavedDir() + L"\\Screenshots\\Windows\\";
    for (const auto& c : waiting) {
        // the newest screenshot the game wrote since the capture was asked for, once it can be opened alone
        WIN32_FIND_DATAW fd;
        std::wstring found;
        FILETIME newest = c->after;
        HANDLE find = FindFirstFileW((folder + L"ScreenShot*.png").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do
                if (CompareFileTime(&fd.ftLastWriteTime, &newest) > 0) {
                    newest = fd.ftLastWriteTime;
                    found = folder + fd.cFileName;
                }
            while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        bool whole = false;
        if (!found.empty()) {
            HANDLE f = CreateFileW(found.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            whole = f != INVALID_HANDLE_VALUE;
            if (whole) CloseHandle(f);
        }
        if (!whole) {
            if (game::Seconds() > c->deadline) {
                std::lock_guard<std::mutex> hold(gLock);
                c->waiting = false;
                c->state = "error: the game wrote no screenshot";
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> hold(gLock);
            c->waiting = false;
        }
        // decoding and scaling a full screenshot takes a few tens of ms: off the game thread
        std::thread([c, found] {
            std::string error;
            const bool ok = MakeThumb(found, c->target, error);
            DeleteFileW(found.c_str());     // the game's screenshot, made only for this
            std::lock_guard<std::mutex> hold(gLock);
            c->state = ok ? "done: " + eng::Narrow(c->target.c_str(), static_cast<int>(c->target.size())) : "error: " + error;
        }).detach();
    }
}

void RemoveOwner(int owner) {
    std::lock_guard<std::mutex> hold(gLock);
    gCaptures.erase(owner);
}

}  // namespace screen
