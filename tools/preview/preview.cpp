// The preview: plugins' windows without the game. Runs the host's real plugin runtime and API (api::Register, the
// plugins' real scripts, settings, storage, the registry) in a plain program, and stands in for the game's screen
// (ui.hpp: "a screen"): it draws the footer and the windows in a browser, or prints them as text.
//
//   preview <plugin folder>... [--port 8790] [--registry live|<registry.json>] [--data <dir>] [--steps <file>]
//
// Browser: open http://localhost:<port>. Editing a plugin's files reloads it. Headless (--steps): runs the steps,
// prints the windows as text wherever a step says `dump`, and exits; non-zero if a step failed.
//
// Everything the plugins write goes under --data (default build/preview): the plugins folder (a fresh copy of the
// folders given, each as the id it is named for, "ballest-" left off), host.log, storage and settings, the registry's
// downloads and icons. Without the game the engine is never found, so game calls return nothing: the footer, the
// cursor, the race, the editor and the HUD are not there, and windows docked into the game's panels don't show.
#include <winsock2.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/host/engine.hpp"
#include "../../src/host/input.hpp"
#include "../../src/host/log.hpp"
#include "../../src/host/plugins.hpp"
#include "../../src/host/registry.hpp"
#include "../../src/host/settings.hpp"
#include "../../src/host/ui.hpp"

namespace fs = std::filesystem;

namespace {

// --- setup -------------------------------------------------------------------------------------------------------

struct Source {
    fs::path from;          // the folder given
    std::string id;         // what it runs as
    fs::file_time_type stamp;
};
std::vector<Source> gSources;
fs::path gRoot, gPlugins, gPage;

std::string IdFor(const fs::path& folder) {
    std::string name = folder.filename().string();
    if (name.empty()) name = folder.parent_path().filename().string();
    return name.rfind("ballest-", 0) == 0 ? name.substr(8) : name;
}

// The newest change to any file in the folder (skipping .git), so an edit anywhere in a plugin reloads it.
fs::file_time_type Stamp(const fs::path& folder) {
    fs::file_time_type newest{};
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->path().filename() == ".git") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) newest = std::max(newest, it->last_write_time(ec));
    }
    return newest;
}

void CopyIn(const Source& s) {
    const fs::path to = gPlugins / s.id;
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to, ec);
    for (auto it = fs::recursive_directory_iterator(s.from, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->path().filename() == ".git") {
            it.disable_recursion_pending();
            continue;
        }
        const fs::path rel = fs::relative(it->path(), s.from, ec);
        if (it->is_directory(ec)) fs::create_directories(to / rel, ec);
        else fs::copy_file(it->path(), to / rel, fs::copy_options::overwrite_existing, ec);
    }
}

// --- text out ------------------------------------------------------------------------------------------------------

std::string Escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string Num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

std::string Color(const ui::Color& c) { return "[" + Num(c.r) + "," + Num(c.g) + "," + Num(c.b) + "," + Num(c.a) + "]"; }

std::string Id(const void* p) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));
    return buf;
}

const char* KindName(ui::Kind k) {
    switch (k) {
        case ui::Kind::Text: return "text";
        case ui::Kind::Button: return "button";
        case ui::Kind::IconButton: return "icon";
        case ui::Kind::Slider: return "slider";
        case ui::Kind::Dropdown: return "dropdown";
        case ui::Kind::Space: return "space";
        case ui::Kind::TextArea: return "textarea";
        case ui::Kind::TextInput: return "input";
        case ui::Kind::Image: return "image";
        case ui::Kind::CheckBox: return "check";
        case ui::Kind::Rect: return "rect";
    }
    return "?";
}

// A text input's box shows what the plugin put in it (pendingValue) once; after that the browser owns what's typed.
// valueSeq tells the page to take the box's text from the model again.
std::map<const ui::Widget*, int> gValueSeq;

// The whole screen as JSON, for the page. Images are paths the page fetches through /file.
std::string ScreenJson() {
    std::string j = "{\"footer\":[";
    bool first = true;
    for (const auto& b : ui::footer::Buttons()) {
        j += std::string(first ? "" : ",") + "{\"id\":\"" + Id(b.get()) + "\",\"label\":\"" + Escape(b->label) + "\"}";
        first = false;
    }
    j += "],\"windows\":[";
    first = true;
    for (const auto& wp : ui::windows::All()) {
        const ui::Window& w = *wp;
        if (w.dock != ui::Dock::Screen) continue;
        j += first ? "" : ",";
        first = false;
        j += "{\"id\":\"" + Id(&w) + "\",\"visible\":" + (w.visible ? "true" : "false") + ",\"z\":" + std::to_string(w.zOrder) +
             ",\"anchor\":[" + Num(w.anchorX) + "," + Num(w.anchorY) + "],\"pivot\":[" + Num(w.pivotX) + "," + Num(w.pivotY) +
             "],\"offset\":[" + Num(w.offsetX) + "," + Num(w.offsetY) + "],\"screen\":[" + Num(w.screenWidth) + "," +
             Num(w.screenHeight) + "],\"rect\":[" + Num(w.rectWidth) + "," + Num(w.rectHeight) + "],\"background\":" +
             Color(w.background) + ",\"radius\":" + Num(w.cornerRadius) + ",\"padding\":[" + Num(w.paddingX) + "," +
             Num(w.paddingY) + "],\"rowGap\":" + Num(w.rowGap) + ",\"sidebar\":" + Num(w.sidebarWidth) +
             ",\"card\":" + Color(w.cardBackground) + ",\"views\":" + std::to_string(w.views) + ",\"shownView\":" +
             std::to_string(w.shownView) + ",\"scrolling\":[";
        for (size_t i = 0; i < w.scrollingViews.size(); ++i) j += (i ? "," : "") + std::to_string(w.scrollingViews[i]);
        j += "],\"rows\":[";
        for (size_t r = 0; r < w.rowView.size(); ++r)
            j += std::string(r ? "," : "") + "[" + std::to_string(w.rowView[r]) + "," + std::to_string(w.rowCard[r]) + "," +
                 (w.rowRetired[r] ? "1" : "0") + "]";
        j += "],\"items\":[";
        bool firstItem = true;
        for (const auto& ip : w.items) {
            const ui::Widget& it = *ip;
            if (it.retired) continue;
            j += firstItem ? "" : ",";
            firstItem = false;
            j += "{\"id\":\"" + Id(&it) + "\",\"kind\":\"" + KindName(it.kind) + "\",\"row\":" + std::to_string(it.row) +
                 ",\"side\":" + (it.inSidebar ? "true" : "false") + ",\"visible\":" + (it.visible ? "true" : "false") +
                 ",\"text\":\"" + Escape(it.text) + "\",\"size\":" + Num(it.size) + ",\"width\":" + Num(it.width) +
                 ",\"height\":" + Num(it.height) + ",\"textWidth\":" + Num(it.textWidth) + ",\"justify\":" +
                 std::to_string(it.justify) + ",\"font\":\"" + Escape(it.font) + "\",\"fill\":" + (it.fill ? "true" : "false") +
                 ",\"gap\":" + Num(it.gapBefore) + ",\"background\":" + Color(it.background) + ",\"color\":" + Color(it.color) +
                 ",\"colorSet\":" + (it.colorSet ? "true" : "false");
            if (it.placed) j += ",\"placed\":[" + Num(it.px) + "," + Num(it.py) + "," + Num(it.pw) + "," + Num(it.ph) + "]";
            switch (it.kind) {
                case ui::Kind::Slider: j += ",\"value\":" + Num(it.value); break;
                case ui::Kind::Dropdown: {
                    j += ",\"selected\":" + std::to_string(it.selected) + ",\"options\":[";
                    for (size_t o = 0; o < it.options.size(); ++o) j += (o ? ",\"" : "\"") + Escape(it.options[o]) + "\"";
                    j += "]";
                    break;
                }
                case ui::Kind::CheckBox: j += std::string(",\"checked\":") + (it.checked ? "true" : "false"); break;
                case ui::Kind::TextInput:
                    j += ",\"value\":\"" + Escape(it.typed) + "\",\"valueSeq\":" + std::to_string(gValueSeq[&it]) +
                         ",\"clear\":" + (it.clearButton ? "true" : "false") + ",\"readOnly\":" + (it.readOnly ? "true" : "false") +
                         ",\"focus\":" + (it.focusRequested ? "true" : "false");
                    break;
                default: break;
            }
            j += "}";
        }
        j += "]}";
    }
    return j + "]}";
}

// The windows on screen as plain text: what a player would read, one row a line. For golden tests, so it leaves
// out colours, sizes and positions.
std::string DumpText() {
    std::string out;
    std::vector<const ui::Window*> shown;
    for (const auto& w : ui::windows::All())
        if (w->visible && w->dock == ui::Dock::Screen) shown.push_back(w.get());
    std::stable_sort(shown.begin(), shown.end(), [](const ui::Window* a, const ui::Window* b) { return a->zOrder < b->zOrder; });
    auto describe = [](const ui::Widget& it) -> std::string {
        switch (it.kind) {
            case ui::Kind::Text: return "\"" + it.text + "\"";
            case ui::Kind::Button: return "[" + it.text + "]";
            case ui::Kind::IconButton: return "[" + it.text + " icon]";
            case ui::Kind::Slider: return "<slider " + Num(it.value) + ">";
            case ui::Kind::Dropdown:
                return "<" + (it.selected >= 0 && it.selected < static_cast<int>(it.options.size()) ? it.options[static_cast<size_t>(it.selected)] : std::string("-")) + " v>";
            case ui::Kind::Space: return "";
            case ui::Kind::TextArea: return "{text area, " + std::to_string(std::count(it.text.begin(), it.text.end(), '\n') + (it.text.empty() ? 0 : 1)) + " lines}";
            case ui::Kind::TextInput: return "{" + (it.typed.empty() ? it.text + "..." : it.typed) + "}";
            case ui::Kind::Image: return "(image)";
            case ui::Kind::CheckBox: return std::string(it.checked ? "[x] " : "[ ] ") + it.text;
            case ui::Kind::Rect: return "";
        }
        return "";
    };
    for (const ui::Window* w : shown) {
        out += "window " + plugins::IdAt(w->owner) + "\n";
        std::string side;
        for (const auto& it : w->items)
            if (!it->retired && it->visible && it->inSidebar) {
                const std::string d = describe(*it);
                if (!d.empty()) side += "    " + d + "\n";
            }
        if (!side.empty()) out += "  sidebar\n" + side;
        int lastCard = -1;
        for (size_t r = 0; r < w->rowView.size(); ++r) {
            if (w->rowRetired[r] || (w->rowView[r] >= 0 && w->rowView[r] != w->shownView)) continue;
            std::string line;
            for (const auto& it : w->items)
                if (!it->retired && it->visible && !it->inSidebar && it->row == static_cast<int>(r)) {
                    const std::string d = describe(*it);
                    if (!d.empty()) line += (line.empty() ? "" : " ") + d;
                }
            if (line.empty()) continue;
            const int card = w->rowCard[r];
            if (card >= 0 && card != lastCard) out += "  --\n";
            lastCard = card;
            out += std::string(card >= 0 ? "    " : "  ") + line + "\n";
        }
    }
    std::string footer;
    for (const auto& b : ui::footer::Buttons()) footer += " [" + b->label + "]";
    if (!footer.empty()) out += "footer" + footer + "\n";
    return out;
}

// --- input from the page ------------------------------------------------------------------------------------------

ui::Widget* FindWidget(const std::string& id) {
    for (const auto& w : ui::windows::All())
        for (const auto& it : w->items)
            if (Id(it.get()) == id && !it->retired) return it.get();
    return nullptr;
}

ui::FooterButton* FindFooter(const std::string& id) {
    for (const auto& b : ui::footer::Buttons())
        if (Id(b.get()) == id) return b.get();
    return nullptr;
}

// One event: "<type>\n<id>\n<value>" (the page sends plain text; values may hold anything but stay on their line
// except for typed text, which takes the rest).
void ApplyInput(const std::string& body) {
    const size_t a = body.find('\n'), b = a == std::string::npos ? a : body.find('\n', a + 1);
    if (b == std::string::npos) return;
    const std::string type = body.substr(0, a), id = body.substr(a + 1, b - a - 1), value = body.substr(b + 1);
    if (type == "key") {
        input::Simulate(std::atoi(value.c_str()));
        return;
    }
    if (type == "footer") {
        if (ui::FooterButton* f = FindFooter(id)) f->clickPending = true;
        return;
    }
    ui::Widget* it = FindWidget(id);
    if (!it) return;
    if (type == "click") it->clickPending = true;
    else if (type == "hover") it->hovered = value == "1";
    else if (type == "slider") {
        it->value = static_cast<float>(std::atof(value.c_str()));
        it->dragging = true;
    } else if (type == "release") it->dragging = false;
    else if (type == "select") {
        const int index = std::atoi(value.c_str());
        if (index != it->selected) {
            it->selected = index;
            it->changedPending = true;
        }
    } else if (type == "check") {
        it->checked = value == "1";
        it->changedPending = true;
    } else if (type == "type") {
        it->typed = value;
        it->pendingValue = value;
    } else if (type == "focus") {
        it->focused = value == "1";
        it->focusRequested = false;
    } else if (type == "submit") {
        it->typed = value;
        if (!value.empty()) {
            it->submitted = value;
            it->submitPending = true;
            if (it->clearOnSubmit) {
                it->typed.clear();
                it->pendingValue.clear();
                ++gValueSeq[it];
            } else {
                it->pendingValue = value;
            }
        }
    } else if (type == "clear") {
        it->typed.clear();
        it->pendingValue.clear();
        it->clearedPending = true;
        ++gValueSeq[it];
    }
}

// The screen's own bookkeeping each frame: what the plugin put in a text box shows; Submit() from the plugin
// submits; everything visible counts as shown (the test hooks look at windows on screen first).
void PresentFrame() {
    for (const auto& w : ui::windows::All()) {
        w->shownVisible = w->visible;
        for (const auto& it : w->items) {
            it->shownVisible = it->visible;
            if (it->kind != ui::Kind::TextInput) continue;
            if (it->valuePending) {
                it->typed = it->pendingValue;
                it->valuePending = false;
                ++gValueSeq[it.get()];
            }
            if (it->submitRequested) {
                it->submitRequested = false;
                if (!it->typed.empty()) {
                    it->submitted = it->typed;
                    it->submitPending = true;
                    if (it->clearOnSubmit) {
                        it->typed.clear();
                        it->pendingValue.clear();
                        ++gValueSeq[it.get()];
                    }
                }
            }
        }
    }
}

// --- a small HTTP server on localhost --------------------------------------------------------------------------------

SOCKET gListen = INVALID_SOCKET;

bool Listen(int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    gListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (gListen == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);          // this computer only
    if (bind(gListen, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(gListen, 16) != 0) return false;
    u_long nonBlocking = 1;
    ioctlsocket(gListen, FIONBIO, &nonBlocking);
    return true;
}

std::string ReadFileBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

void Respond(SOCKET c, const char* status, const char* type, const std::string& body) {
    std::string head = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    head += body;
    for (size_t sent = 0; sent < head.size();) {
        const int n = send(c, head.data() + sent, static_cast<int>(head.size() - sent), 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

// Only files under the preview's own folder (plugin icons, the registry's cached icons) or the game's fonts a page
// stands in for.
bool Servable(const fs::path& p) {
    std::error_code ec;
    const fs::path full = fs::weakly_canonical(p, ec), root = fs::weakly_canonical(gRoot, ec);
    const std::string ext = full.extension().string();
    if ((ext != ".png" && ext != ".jpg" && ext != ".jpeg") || !fs::is_regular_file(full, ec)) return false;
    const auto rel = full.lexically_relative(root).string();
    return !rel.empty() && rel.rfind("..", 0) != 0;
}

void Serve(SOCKET c) {
    // A request is small; read until the headers and the body announced are in.
    std::string req;
    char buf[8192];
    DWORD timeout = 1000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    for (;;) {
        const int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
        const size_t end = req.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        size_t length = 0;
        const size_t cl = req.find("Content-Length: ");
        if (cl != std::string::npos && cl < end) length = static_cast<size_t>(std::atoll(req.c_str() + cl + 16));
        if (req.size() >= end + 4 + length) break;
    }
    const size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return;
    const std::string method = req.substr(0, sp1), target = req.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t bodyAt = req.find("\r\n\r\n");
    const std::string body = bodyAt == std::string::npos ? "" : req.substr(bodyAt + 4);
    const std::string path = target.substr(0, target.find('?'));
    const std::string query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
    if (path == "/" || path == "/index.html") {
        Respond(c, "200 OK", "text/html; charset=utf-8", ReadFileBytes(gPage));       // read each time: edits show on reload
    } else if (path == "/screen") {
        Respond(c, "200 OK", "application/json", ScreenJson());
    } else if (path == "/input" && method == "POST") {
        // Several events, separated by a line holding only \x1e (record separator).
        size_t at = 0;
        while (at <= body.size()) {
            const size_t next = body.find("\n\x1e\n", at);
            ApplyInput(body.substr(at, next == std::string::npos ? std::string::npos : next - at));
            if (next == std::string::npos) break;
            at = next + 3;
        }
        Respond(c, "200 OK", "text/plain", "ok");
    } else if (path == "/file" && query.rfind("p=", 0) == 0) {
        const fs::path p = fs::path(eng::Widen(UrlDecode(query.substr(2))));
        if (Servable(p)) Respond(c, "200 OK", p.extension() == ".png" ? "image/png" : "image/jpeg", ReadFileBytes(p));
        else Respond(c, "404 Not Found", "text/plain", "not served");
    } else if (path == "/font") {
        // The game's display font, if a copy was put in tools/preview/fonts (it is the game's, so not in the repo).
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(gPage.parent_path() / "fonts", ec))
            if (e.path().extension() == ".ttf" || e.path().extension() == ".otf") {
                Respond(c, "200 OK", "font/ttf", ReadFileBytes(e.path()));
                return;
            }
        Respond(c, "404 Not Found", "text/plain", "no font");
    } else if (path == "/text") {
        Respond(c, "200 OK", "text/plain; charset=utf-8", DumpText());
    } else if (path == "/log") {
        std::string text;
        const size_t count = hostlog::LineCount();
        for (size_t i = count > 200 ? count - 200 : 0; i < count; ++i) text += hostlog::Line(i) + "\n";
        Respond(c, "200 OK", "text/plain; charset=utf-8", text);
    } else {
        Respond(c, "404 Not Found", "text/plain", "no such page");
    }
}

void PollHttp() {
    if (gListen == INVALID_SOCKET) return;
    for (int i = 0; i < 32; ++i) {
        SOCKET c = accept(gListen, nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        u_long blocking = 0;
        ioctlsocket(c, FIONBIO, &blocking);
        Serve(c);
        closesocket(c);
    }
}

// --- the frame -----------------------------------------------------------------------------------------------------

constexpr double kFrameSeconds = 1.0 / 30;
double gLastWatch = 0;

double Now() {
    static LARGE_INTEGER frequency{}, start{};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
        start = now;
    }
    return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// A plugin whose files changed is copied in again and reloaded.
void Watch() {
    if (Now() - gLastWatch < 0.5) return;
    gLastWatch = Now();
    for (auto& s : gSources) {
        const auto stamp = Stamp(s.from);
        if (stamp == s.stamp) continue;
        s.stamp = stamp;
        hostlog::Info("preview: " + s.id + " changed; reloading it");
        plugins::Unload(s.id);
        CopyIn(s);
        if (!plugins::Load(s.id)) hostlog::Error("preview: " + s.id + " did not load (see above)");
    }
}

void Frame() {
    input::Frame();
    registry::Frame();
    plugins::Frame(static_cast<float>(kFrameSeconds));
    PresentFrame();
}

void Sleep(double seconds) { ::Sleep(static_cast<DWORD>(std::max(0.0, seconds) * 1000)); }

// --- steps (headless) ---------------------------------------------------------------------------------------------

int gFailures = 0;

void Fail(const std::string& line, const std::string& why) {
    std::fprintf(stderr, "step failed: %s (%s)\n", line.c_str(), why.c_str());
    ++gFailures;
}

void RunFrames(double seconds) {
    const double until = Now() + seconds;
    do {
        const double start = Now();
        Frame();
        Sleep(kFrameSeconds - (Now() - start));
    } while (Now() < until);
}

int KeyCode(const std::string& name) {
    static const std::map<std::string, int> names = {{"escape", 0x1B}, {"enter", 0x0D}, {"left", 0x25}, {"up", 0x26},
                                                     {"right", 0x27}, {"down", 0x28}, {"space", 0x20}, {"tab", 0x09}};
    std::string lower = name;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (auto it = names.find(lower); it != names.end()) return it->second;
    if (lower.size() == 1 && std::isalnum(static_cast<unsigned char>(lower[0]))) return std::toupper(static_cast<unsigned char>(lower[0]));
    return std::atoi(name.c_str());
}

// Steps, one a line (# comments): wait <seconds> | click <label> | type [@hint|]<text> | submit [@hint|]<text> |
// select <first option> <index> | slider <0..1> | press <key> | setting <plugin> <variable> <value> | dump
// Each step is followed by a few frames, so what it changed has been drawn before the next one.
void RunSteps(const fs::path& file) {
    std::ifstream f(file);
    if (!f) {
        Fail(file.string(), "no such steps file");
        return;
    }
    RunFrames(1.0);             // plugins start, their windows are built
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t sp = line.find(' ');
        const std::string verb = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
        bool ok = true;
        if (verb == "wait") RunFrames(std::atof(arg.c_str()));
        else if (verb == "dump") std::fputs(DumpText().c_str(), stdout), std::fputs("\n", stdout);
        else if (verb == "click") ok = ui::SimulateClick(arg);
        else if (verb == "type") ok = ui::SimulateType(arg);
        else if (verb == "submit") ok = ui::SimulateSubmit(arg);
        else if (verb == "slider") ui::SimulateSlider(static_cast<float>(std::atof(arg.c_str())));
        else if (verb == "press") input::Simulate(KeyCode(arg));
        else if (verb == "select") {
            const size_t cut = arg.rfind(' ');
            ok = cut != std::string::npos && ui::SimulateSelect(arg.substr(0, cut), std::atoi(arg.c_str() + cut + 1));
        } else if (verb == "setting") {
            std::istringstream in(arg);
            std::string plugin, variable, value;
            in >> plugin >> variable;
            std::getline(in >> std::ws, value);
            ok = false;
            const auto& list = settings::List();
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i].pluginId == plugin && list[i].variable == variable) ok = settings::Set(i, value);
        } else {
            Fail(line, "unknown step");
            continue;
        }
        if (!ok) Fail(line, "nothing to do it to");
        if (verb != "wait" && verb != "dump") RunFrames(0.5);
    }
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8790;
    std::string registry, steps;
    fs::path data = "build/preview";
    std::vector<fs::path> folders;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--registry" && i + 1 < argc) registry = argv[++i];
        else if (a == "--data" && i + 1 < argc) data = argv[++i];
        else if (a == "--steps" && i + 1 < argc) steps = argv[++i];
        else folders.push_back(a);
    }
    if (folders.empty()) {
        std::fputs("preview <plugin folder>... [--port 8790] [--registry live|<registry.json>] [--data <dir>] [--steps <file>]\n", stderr);
        return 2;
    }
    std::error_code ec;
    gRoot = fs::absolute(data, ec);
    gPage = fs::absolute(fs::path(argv[0]).parent_path() / ".." / "tools" / "preview" / "page.html", ec);
    if (!fs::exists(gPage)) gPage = fs::absolute("tools/preview/page.html", ec);
    // The host keeps everything under %LOCALAPPDATA%\Ballest\Saved\PluginManager: point that at the preview's folder.
    const fs::path local = gRoot / "local";
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.wstring().c_str());
    hostlog::Open();
    const fs::path hostData = hostlog::DataDir();
    gPlugins = hostData / "plugins";
    fs::remove_all(gPlugins, ec);
    fs::create_directories(gPlugins, ec);
    fs::remove(hostData / "off.txt", ec);
    for (const auto& folder : folders) {
        Source s{fs::absolute(folder, ec), IdFor(fs::absolute(folder, ec)), {}};
        s.stamp = Stamp(s.from);
        CopyIn(s);
        gSources.push_back(s);
    }
    // The registry: the live one by default (network), or a local registry.json (no network: for tests).
    fs::remove(hostData / "registry_url.txt", ec);
    if (!registry.empty() && registry != "live") {
        std::string url = "file:///" + fs::absolute(registry, ec).generic_string();
        std::ofstream(hostData / "registry_url.txt") << url << "\n";
    }
    hostlog::Info(std::string("preview of plugin host ") + plugins::kHostVersion + ", no game");
    plugins::LoadAll(gPlugins.wstring());
    registry::Refresh();

    if (!steps.empty()) {
        _setmode(_fileno(stdout), _O_BINARY);      // plain newlines: the expected text reads the same on every system
        RunSteps(steps);
        return gFailures == 0 ? 0 : 1;
    }
    if (!Listen(port)) {
        std::fprintf(stderr, "could not listen on localhost:%d\n", port);
        return 1;
    }
    std::printf("preview on http://localhost:%d  (data in %s)\n", port, gRoot.string().c_str());
    std::fflush(stdout);
    for (;;) {
        const double start = Now();
        PollHttp();
        Watch();
        Frame();
        Sleep(kFrameSeconds - (Now() - start));
    }
}
