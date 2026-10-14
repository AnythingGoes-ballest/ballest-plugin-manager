// HTTPS requests for plugins (Net::), narrow on purpose:
//   * only to the hosts the plugin names in its info.toml ([net] domains = ["example.workers.dev"]), matched exactly
//     (no subdomains, no IP addresses, port 443 only, https only); a redirect is never followed;
//   * GET or POST; the only request headers are Content-Type: application/json (for a POST body), an optional
//     Authorization: Bearer <token>, the host's own User-Agent and X-Ballest-Plugin: <plugin id>; in a sandboxed test copy also
//     X-Ballest-Sandbox: 1, so the server can keep test traffic apart from real records;
//   * a POST body up to 1 MB (room for a run's replay), a response up to 2 MB, a timeout of 1..30 s;
//   * up to 4 requests under way per plugin and 30 started per minute.
// Requests run on worker threads; the plugin polls for the result. In a sandboxed test copy the game's own network
// stays blocked (sandbox.cpp hooks the game's imports); these requests are made by the host from this DLL, so the
// declared domains are the copy's deliberate allow-list and nothing else is opened. Game thread only (the API side).
#pragma once
#include <string>
#include <vector>

namespace pluginnet {

// Whether a [net] domain is a public host name: no IP addresses, localhost, *.local and the like, or single-label names
// (refused when the manifest is read and again for every request). `why` says why not.
bool DomainAllowed(const std::string& domain, std::string* why);

// A request id (> 0), or 0 if refused (LastRefusal says why, and it is logged).
int Start(int owner, const std::string& pluginId, const std::vector<std::string>& domains, const std::string& method, const std::string& url,
          const std::string& body, const std::string& bearer, int timeoutMs);
std::string LastRefusal();
bool Done(int owner, int id);
int Status(int owner, int id);                  // the HTTP status once done, 0 when no response came (Error says why)
std::string Body(int owner, int id);
std::string Error(int owner, int id);
void Forget(int owner, int id);
void RemoveOwner(int owner);

}  // namespace pluginnet
