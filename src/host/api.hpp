// The script API plugins see: Log, Host, Plugins, UI, Input, Replay. Everything a plugin can do goes through
// these registrations; plugins have no other way to reach the game.
#pragma once

class asIScriptEngine;

namespace api {

void Register(asIScriptEngine* engine);

}  // namespace api
