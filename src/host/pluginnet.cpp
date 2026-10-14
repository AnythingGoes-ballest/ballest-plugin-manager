#include "pluginnet.hpp"
#if __has_include("sandbox.hpp")
#include "sandbox.hpp"
#define HOST_HAS_SANDBOX 1
#endif

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "log.hpp"
#include "net.hpp"

namespace pluginnet {
namespace {

constexpr size_t kMaxBody = 1024 * 1024, kMaxResponse = 2 * 1024 * 1024, kMaxUrl = 2048, kMaxBearer = 4096;
constexpr int kMaxInFlight = 4, kMaxPerMinute = 30;
constexpr ULONGLONG kKeepDoneMs = 5 * 60 * 1000;    // a result nobody asked for again is dropped after this

struct Result {
    int owner = -1;
    bool done = false;
    int status = 0;
    std::string body, error;
    ULONGLONG doneAt = 0;
};

std::mutex gLock;                                   // guards gResults (worker threads fill them in)
std::map<int, std::shared_ptr<Result>> gResults;
int gNextId = 1;
std::map<int, std::deque<ULONGLONG>> gStarts;       // per owner: when its recent requests started (game thread)
std::string gLastRefusal;
std::set<std::string> gLoggedAllow;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The URL's host when it is https://<host>[:443]/..., or "" (anything else is refused).
std::string HostOf(const std::string& url) {
    if (url.size() > kMaxUrl || Lower(url.substr(0, 8)) != "https://") return "";
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f || c == '\\') return "";
    const size_t end = url.find_first_of("/?#", 8);
    std::string authority = url.substr(8, end == std::string::npos ? std::string::npos : end - 8);
    if (authority.find('@') != std::string::npos) return "";
    if (const size_t colon = authority.find(':'); colon != std::string::npos) {
        if (authority.substr(colon + 1) != "443") return "";
        authority.resize(colon);
    }
    return Lower(authority);
}

}  // namespace

// A host a plugin may declare: a public DNS name of two labels or more. Never an IP address (v4 in any notation, v6),
// localhost, a *.local / *.localhost / *.internal / *.lan / *.home.arpa name or a single-label name (the player's own
// computer and network).
bool DomainAllowed(const std::string& domain, std::string* why) {
    const std::string d = Lower(domain);
    auto refuse = [&](const std::string& reason) {
        if (why) *why = "'" + domain + "': " + reason;
        return false;
    };
    if (d.empty() || d.size() > 253) return refuse("not a host name");
    if (d.find_first_of(":[]") != std::string::npos) return refuse("an IPv6 address or a port");
    for (char c : d)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return refuse("not a host name");
    if (d.front() == '.' || d.back() == '.' || d.find("..") != std::string::npos) return refuse("not a host name");
    if (d.find('.') == std::string::npos) return refuse("a single-label name (a local network name)");
    // the last label all digits (or hex 0x...): an IPv4 address in some notation, never a top-level domain
    const std::string last = d.substr(d.rfind('.') + 1);
    if (last.find_first_not_of("0123456789") == std::string::npos || last.rfind("0x", 0) == 0) return refuse("an IP address");
    for (const char* local : {"localhost", "local", "localdomain", "internal", "lan", "home", "intranet", "corp", "private"})
        if (last == local) return refuse("a local network name");
    if (d.size() >= 10 && d.compare(d.size() - 10, 10, ".home.arpa") == 0) return refuse("a local network name");
    if (d.size() >= 13 && d.compare(d.size() - 13, 13, ".in-addr.arpa") == 0) return refuse("an IP address");
    return true;
}

namespace {

bool BearerOk(const std::string& token) {
    if (token.size() > kMaxBearer) return false;
    return std::all_of(token.begin(), token.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-' || c == '~' || c == '+' || c == '/' || c == '=';
    });
}

int Refuse(const std::string& pluginId, const std::string& why) {
    gLastRefusal = why;
    hostlog::Write("warn", pluginId, "Net: refused: " + why);
    return 0;
}

std::shared_ptr<Result> Find(int owner, int id) {
    std::lock_guard<std::mutex> hold(gLock);
    auto it = gResults.find(id);
    if (it == gResults.end() || it->second->owner != owner) return nullptr;
    return it->second;
}

void DropStale() {
    const ULONGLONG now = GetTickCount64();
    for (auto it = gResults.begin(); it != gResults.end();)
        if (it->second->done && now - it->second->doneAt > kKeepDoneMs) it = gResults.erase(it);
        else ++it;
}

}  // namespace

int Start(int owner, const std::string& pluginId, const std::vector<std::string>& domains, const std::string& method, const std::string& url,
          const std::string& body, const std::string& bearer, int timeoutMs) {
    if (owner < 0) return Refuse(pluginId, "not called from a plugin");
    if (method != "GET" && method != "POST") return Refuse(pluginId, "only GET and POST");
    const std::string host = HostOf(url);
    if (host.empty()) return Refuse(pluginId, "not a plain https:// URL on port 443: " + url.substr(0, 120));
    const bool declared = std::any_of(domains.begin(), domains.end(), [&](const std::string& d) { return Lower(d) == host; });
    if (!declared) return Refuse(pluginId, host + " is not in this plugin's [net] domains (info.toml)");
    if (std::string why; !DomainAllowed(host, &why)) return Refuse(pluginId, "not a public host: " + why);
    if (body.size() > kMaxBody) return Refuse(pluginId, "body over 1 MB");
    if (method == "GET" && !body.empty()) return Refuse(pluginId, "a GET has no body");
    if (!bearer.empty() && !BearerOk(bearer)) return Refuse(pluginId, "the bearer token has characters a token can't have");
    std::lock_guard<std::mutex> hold(gLock);
    DropStale();
    int inFlight = 0;
    for (const auto& [id, r] : gResults)
        if (r->owner == owner && !r->done) ++inFlight;
    if (inFlight >= kMaxInFlight) {
        gLastRefusal = "4 requests are already under way";
        hostlog::Write("warn", pluginId, "Net: refused: " + gLastRefusal);
        return 0;
    }
    auto& starts = gStarts[owner];
    const ULONGLONG now = GetTickCount64();
    while (!starts.empty() && now - starts.front() > 60000) starts.pop_front();
    if (static_cast<int>(starts.size()) >= kMaxPerMinute) {
        gLastRefusal = "30 requests a minute at most";
        hostlog::Write("warn", pluginId, "Net: refused: " + gLastRefusal);
        return 0;
    }
    starts.push_back(now);
    net::Request request;
    request.method = method;
    request.url = url;
    request.body = body;
    request.timeoutMs = std::clamp(timeoutMs, 1000, 30000);
    request.maxResponse = kMaxResponse;
    request.headers = "X-Ballest-Plugin: " + pluginId + "\r\n";
    if (method == "POST") request.headers += "Content-Type: application/json\r\n";
    if (!bearer.empty()) request.headers += "Authorization: Bearer " + bearer + "\r\n";
#ifdef HOST_HAS_SANDBOX
    if (sandbox::On()) {
        // The test copy's allow-list: only this plugin's declared hosts, marked as test traffic. The game's own
        // connections, Steam's uploads and every other block are untouched.
        request.headers += "X-Ballest-Sandbox: 1\r\n";
        if (gLoggedAllow.insert(pluginId + "|" + host).second)
            hostlog::Info("sandbox: plugin " + pluginId + " may reach " + host + " (declared in its info.toml; sent as test traffic); every other block stays");
    }
#endif
    const int id = gNextId++;
    auto result = std::make_shared<Result>();
    result->owner = owner;
    gResults[id] = result;
    std::thread([request, result] {
        int status = 0;
        std::string response, error;
        const bool ok = net::Send(request, status, response, error);
        std::lock_guard<std::mutex> done(gLock);
        result->status = ok ? status : 0;
        result->body = ok ? std::move(response) : std::string();
        result->error = ok ? std::string() : error;
        result->done = true;
        result->doneAt = GetTickCount64();
    }).detach();
    return id;
}

std::string LastRefusal() { return gLastRefusal; }

bool Done(int owner, int id) {
    auto r = Find(owner, id);
    std::lock_guard<std::mutex> hold(gLock);
    return r && r->done;
}

int Status(int owner, int id) {
    auto r = Find(owner, id);
    std::lock_guard<std::mutex> hold(gLock);
    return r && r->done ? r->status : 0;
}

std::string Body(int owner, int id) {
    auto r = Find(owner, id);
    std::lock_guard<std::mutex> hold(gLock);
    return r && r->done ? r->body : std::string();
}

std::string Error(int owner, int id) {
    auto r = Find(owner, id);
    std::lock_guard<std::mutex> hold(gLock);
    if (!r) return "no such request";
    return r->done ? r->error : std::string();
}

void Forget(int owner, int id) {
    std::lock_guard<std::mutex> hold(gLock);
    auto it = gResults.find(id);
    if (it != gResults.end() && it->second->owner == owner) gResults.erase(it);
}

void RemoveOwner(int owner) {
    std::lock_guard<std::mutex> hold(gLock);
    for (auto it = gResults.begin(); it != gResults.end();)
        if (it->second->owner == owner) it = gResults.erase(it);
        else ++it;
    gStarts.erase(owner);
}

}  // namespace pluginnet
