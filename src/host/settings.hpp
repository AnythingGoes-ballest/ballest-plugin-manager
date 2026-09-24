// Plugin settings, declared the way Openplanet declares them: a global variable with a [Setting] tag.
//
//   [Setting name="Timer size" min=16 max=160 description="Height of the time, in pixels"]
//   float TimeSize = 56;
//
// Attributes: name, description, min and max (numbers; both together give a slider), hidden (kept and saved but
// not shown). Types: bool, int, uint, float, double, string. The value the variable starts with is the default.
// Saved per plugin in Storage under "setting.<variable>"; a setting at its default is not saved, so a changed
// default reaches everyone who never changed it. The plugin manager shows them and edits them; after a change the
// plugin's `void OnSettingsChanged()` runs before its next Update.
#pragma once
#include <string>
#include <vector>

class CScriptBuilder;
class asIScriptModule;

namespace settings {

enum class Kind { Bool, Int, UInt, Float, Double, String };

struct Setting {
    int plugin = -1;
    std::string pluginId, variable, name, description;
    Kind kind = Kind::Bool;
    bool hasRange = false, hidden = false;
    double min = 0, max = 0;
    std::string defaultValue;
    void* address = nullptr;            // the script global; valid while the plugin's module exists
};

// After the plugin's module is built and before Main: reads its [Setting] globals and applies saved values.
void Collect(int plugin, const std::string& pluginId, asIScriptModule* module, CScriptBuilder& builder);
void Forget(int plugin);                // the plugin is unloaded: its variables are gone
void Frame();                           // writes changed values to storage, a few times a second at most

const std::vector<Setting>& List();
std::string Get(size_t index);
bool Set(size_t index, const std::string& value);   // parsed, clamped to min..max, written into the plugin
void Reset(size_t index);
bool IsDefault(size_t index);
bool TakeChanged(int plugin);           // true once after any of the plugin's settings changed

}  // namespace settings
