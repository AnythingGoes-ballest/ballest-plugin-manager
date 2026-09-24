// Plugin Manager: a "plugins" entry in the game's footer (main menu and inside maps) that opens a panel listing
// every plugin the host found, with its version and status. The list refreshes every second while the panel is
// open, so a plugin that stops (an error, or overrunning its time budget) shows as stopped straight away.
//
// The panel's "open" button opens the plugin manager menu: a window over 90% of the screen with a sidebar of views.
//   console   the host log as it is written, and host commands (find, props, functions, ...; the full list is in
//             src/host/testchannel.hpp): type one and press Enter, or click run
//   plugins   every plugin in the registry plus the ones installed here, one card each: icon, name, version,
//             author, description, and install / update / remove / github buttons. Installs and removals take
//             effect straight away.
//             When the registry names a newer plugin manager (the host and this plugin), it says so here and in the
//             footer panel, with an "update plugin manager" button; the update finishes when the game restarts.
//   settings  each plugin's [Setting] variables: a slider for a number with min and max, on/off for a bool, a
//             text box otherwise, and reset; plus "reset position" for a plugin with windows that can be dragged.

UI::FooterButton@ button;
UI::Panel@ panel;
UI::FooterButton@ openButton;
double lastRefresh = 0;

const float NAV_R = 0.10f, NAV_G = 0.10f, NAV_B = 0.12f;           // a sidebar entry
const float NAV_SELECTED_R = 0.30f, NAV_SELECTED_G = 0.30f, NAV_SELECTED_B = 0.36f;
const int DESCRIPTION_LENGTH = 120;

UI::Window@ menu;
UI::Button@ consoleNav;
UI::Button@ pluginsNav;
UI::Button@ settingsNav;
UI::Button@ folderButton;
UI::Button@ closeButton;
int consoleView = 0;            // the first view, the one rows go into before StartView
int pluginsView = -1;
int settingsView = -1;
int shownView = 0;

// console
UI::TextArea@ logView;
UI::TextInput@ commandInput;
UI::Button@ runButton;
uint shownLogLines = 0;
const uint LOG_LINES_SHOWN = 400;

// plugins: the cards on screen and what their buttons do
UI::Button@ refreshButton;
array<UI::Button@> cardButtons;
array<string> cardActions;      // "install:<id>", "remove:<id>" or "open:<url>"
string shownCards;              // what the cards were built from; rebuilt when it changes
double lastCardCheck = 0;

// settings: every control and the setting (its index in Settings::) it edits
array<UI::Button@> toggles;
array<uint> toggleSetting;
array<UI::Slider@> sliders;
array<uint> sliderSetting;
array<UI::TextInput@> inputs;
array<uint> inputSetting;
array<UI::Text@> values;
array<uint> valueSetting;
array<UI::Button@> resets;
array<uint> resetSetting;
array<UI::Button@> positionResets;
array<string> positionPlugin;
string shownSettings;           // what the settings view was built from

void Main()
{
    Log::Info("plugin manager started on host " + Host::Version());
    @button = UI::AddFooterButton("plugins");
    @panel = UI::CreatePanel();
    panel.title = "plugins";
    @openButton = panel.AddButton("open");
    Refresh();
    BuildMenu();
}

// A newer plugin manager in the registry than the one running, or "".
string HostUpdate()
{
    string latest = Registry::HostVersion();
    return latest != "" && CompareVersions(latest, Host::Version()) > 0 ? latest : "";
}

void Refresh()
{
    panel.Clear();
    if (HostUpdate() != "")
        panel.AddLine(Plugins::HostUpdateState() == "restart" ? "plugin manager " + HostUpdate() + " installed: restart the game"
                                                              : "plugin manager " + HostUpdate() + " available: open > plugins");
    for (uint i = 0; i < Plugins::Count(); i++)
        panel.AddLine(Plugins::Name(i) + "   " + Plugins::Version(i) + "   " + Plugins::Status(i));
    lastRefresh = Host::Time();
}

//   plugin manager  | console                                        | plugins        3 in the registry [refresh]
//   [console]       | +------------------------------------------+   | [icon] Replay Manager  0.1.0   status [..]
//   [plugins]       | | host log                                 |   | [icon] Grind Timer     0.1.0   status [..]
//                   | +------------------------------------------+   |
//   [open folder]   | >  [ command                         ] [run]   |
//   [close]         |                                                |
// Widths and heights of 0 fill the space left over.
void BuildMenu()
{
    @menu = UI::CreateWindow();
    menu.SetScreenSize(0.9f, 0.9f);
    menu.SetBackground(0.02f, 0.02f, 0.03f, 0.94f);
    menu.SetBlocksClicks(true);         // the game's menu underneath must not get clicks through it
    menu.zOrder = 500;                  // in front of every other plugin's windows (they default to 100)
    menu.visible = false;

    menu.StartSidebar(220);
    menu.AddText("plugin manager", 24);
    @consoleNav = menu.AddButton("console");
    @pluginsNav = menu.AddButton("plugins");
    @settingsNav = menu.AddButton("settings");
    menu.AddText(" ", 16);
    @folderButton = menu.AddButton("open plugins folder");
    @closeButton = menu.AddButton("close");

    menu.StartMain();
    menu.AddText("console", 24);
    menu.NewRow();
    @logView = menu.AddTextArea(0, 0, 17);
    menu.NewRow();
    menu.AddText(">", 18);
    @commandInput = menu.AddTextInput(0, "find PlayerController, props <Class>, functions <Class>, ...", 20);
    @runButton = menu.AddButton("run");

    pluginsView = menu.StartView();
    BuildCards();
    settingsView = menu.StartView();
    BuildSettings();
    ShowView(consoleView);
}

void ShowView(int view)
{
    shownView = view;
    menu.ShowView(view);
    Highlight(consoleNav, view == consoleView);
    Highlight(pluginsNav, view == pluginsView);
    Highlight(settingsNav, view == settingsView);
    if (view == consoleView)
        commandInput.Focus();
}

void Highlight(UI::Button@ nav, bool selected)
{
    if (selected)
        nav.SetBackground(NAV_SELECTED_R, NAV_SELECTED_G, NAV_SELECTED_B, 1);
    else
        nav.SetBackground(NAV_R, NAV_G, NAV_B, 1);
}

void OpenMenu()
{
    panel.visible = false;
    menu.visible = true;
    UI::SetCursorVisible(true);
    ShowView(shownView);
    shownLogLines = 0;              // show the log as it is now
    Log::Info("menu opened");
}

void CloseMenu()
{
    menu.visible = false;
    UI::SetCursorVisible(false);
    Log::Info("menu closed");
}

// --- console ----------------------------------------------------------------------------------------------------

// The last LOG_LINES_SHOWN lines of the host log, oldest first.
void ShowLog()
{
    uint count = Log::LineCount();
    uint first = count > LOG_LINES_SHOWN ? count - LOG_LINES_SHOWN : 0;
    string text;
    for (uint i = first; i < count; i++)
    {
        if (i > first)
            text += "\n";
        text += Log::Line(i);
    }
    logView.text = text;
    shownLogLines = count;
}

void UpdateConsole()
{
    if (runButton.Clicked())
        commandInput.Submit();
    if (commandInput.Submitted())
    {
        Log::Info("> " + commandInput.text);
        Console::Run(commandInput.text);
    }
    if (Log::LineCount() != shownLogLines)
        ShowLog();
}

// --- plugins ------------------------------------------------------------------------------------------------------

int RegistryIndex(const string &in id)
{
    for (uint i = 0; i < Registry::Count(); i++)
        if (Registry::Id(i) == id)
            return int(i);
    return -1;
}

int InstalledIndex(const string &in id)
{
    for (uint i = 0; i < Plugins::Count(); i++)
        if (Plugins::Id(i) == id)
            return int(i);
    return -1;
}

// Every plugin to show: the registry's, then installed ones it doesn't list (local plugins).
array<string> CardIds()
{
    array<string> ids;
    for (uint i = 0; i < Registry::Count(); i++)
        ids.insertLast(Registry::Id(i));
    for (uint i = 0; i < Plugins::Count(); i++)
        if (RegistryIndex(Plugins::Id(i)) < 0)
            ids.insertLast(Plugins::Id(i));
    return ids;
}

// Everything the cards show, as one string: when it changes, the cards are rebuilt.
string CardState()
{
    string state = Registry::State() + "|host:" + HostUpdate() + ":" + Plugins::HostUpdateState();
    array<string> ids = CardIds();
    for (uint n = 0; n < ids.length(); n++)
    {
        int r = RegistryIndex(ids[n]);
        int p = InstalledIndex(ids[n]);
        state += "|" + ids[n] + ":" + Plugins::Pending(ids[n]);
        if (r >= 0)
            state += ":" + Registry::Version(r) + ":" + Registry::Icon(r);
        if (p >= 0)
            state += ":" + Plugins::Version(p) + ":" + Plugins::Status(p);
    }
    return state;
}

string Shorten(const string &in text, int length)
{
    return int(text.length()) <= length ? text : text.substr(0, length - 3) + "...";
}

// "1.10.0" > "1.9.2"
int CompareVersions(const string &in a, const string &in b)
{
    array<string>@ x = a.split(".");
    array<string>@ y = b.split(".");
    for (uint i = 0; i < x.length() || i < y.length(); i++)
    {
        int64 nx = i < x.length() ? parseInt(x[i]) : 0;
        int64 ny = i < y.length() ? parseInt(y[i]) : 0;
        if (nx != ny)
            return nx < ny ? -1 : 1;
    }
    return 0;
}

void AddCardButton(const string &in label, const string &in action)
{
    cardButtons.insertLast(menu.AddButton(label));
    cardActions.insertLast(action);
}

void AddCard(const string &in id)
{
    int r = RegistryIndex(id);
    int p = InstalledIndex(id);
    bool installed = p >= 0;
    string name = r >= 0 ? Registry::Name(r) : Plugins::Name(p);
    string author = r >= 0 ? Registry::Author(r) : Plugins::Author(p);
    string description = r >= 0 ? Registry::Description(r) : Plugins::Description(p);
    string version = installed ? Plugins::Version(p) : Registry::Version(r);
    string icon = installed ? Plugins::Icon(p) : "";
    if (icon == "" && r >= 0)
        icon = Registry::Icon(r);
    if (icon == "")
        icon = Plugins::DefaultIcon();

    menu.NewRow();
    menu.AddImage(icon, 72, 72);
    string heading = name + "   " + version + (author != "" ? "   by " + author : "");
    menu.AddText(heading + "\n" + Shorten(description == "" ? "(no description)" : description, DESCRIPTION_LENGTH), 17);
    menu.AddSpace(0);

    string pending = Plugins::Pending(id);
    bool newer = installed && r >= 0 && CompareVersions(Registry::Version(r), Plugins::Version(p)) > 0;
    string status;
    if (pending != "")
        status = pending;
    else if (!installed)
        status = "not installed";
    else if (newer)
        status = "update: " + Registry::Version(r);
    else
        status = "installed";
    UI::Text@ statusText = menu.AddText(status, 16);
    if (pending.findFirst("error") == 0)
        statusText.SetColor(1.0f, 0.45f, 0.4f, 1);
    else if (newer)
        statusText.SetColor(0.55f, 0.85f, 0.0f, 1);
    else
        statusText.SetColor(0.7f, 0.7f, 0.75f, 1);

    bool busy = pending == "installing" || pending == "removing";
    if (!busy && r >= 0 && !installed)
        AddCardButton("install", "install:" + id);
    if (!busy && newer)
        AddCardButton("update", "install:" + id);
    if (!busy && installed && !Plugins::Essential(p))
        AddCardButton("remove", "remove:" + id);
    if (r >= 0)
        AddCardButton("github", "open:" + Registry::Page(r));
}

// The plugin manager itself (the host and this plugin), when the registry has a newer one.
void AddHostUpdateRow()
{
    string latest = HostUpdate();
    if (latest == "")
        return;
    string state = Plugins::HostUpdateState();
    menu.NewRow();
    menu.AddImage(Plugins::Icon(uint(InstalledIndex("plugin-manager"))), 48, 48);
    string text;
    if (state == "restart")
        text = "Plugin manager " + latest + " is installed: restart the game to finish.";
    else if (state == "downloading")
        text = "Updating the plugin manager to " + latest + "...";
    else
        text = "Plugin manager " + latest + " is available (you have " + Host::Version() + ").";
    UI::Text@ line = menu.AddText(text + (state.findFirst("error") == 0 ? "\n" + state : ""), 18);
    if (state.findFirst("error") == 0)
        line.SetColor(1.0f, 0.45f, 0.4f, 1);
    else
        line.SetColor(0.55f, 0.85f, 0.0f, 1);
    menu.AddSpace(0);
    if (state != "restart" && state != "downloading")
        AddCardButton("update plugin manager", "host:" + latest);
}

void BuildCards()
{
    menu.ClearView(pluginsView);
    cardButtons.resize(0);
    cardActions.resize(0);

    menu.AddText("plugins", 24);
    menu.AddSpace(0);
    string state = Registry::State();
    string summary = state == "ready" ? Registry::Count() + " in the registry" : "registry: " + (state == "" ? "not loaded" : state);
    menu.AddText(summary, 16).SetColor(0.7f, 0.7f, 0.75f, 1);
    @refreshButton = menu.AddButton("refresh");
    AddHostUpdateRow();

    array<string> ids = CardIds();
    for (uint n = 0; n < ids.length(); n++)
        AddCard(ids[n]);
    shownCards = CardState();
}

void UpdatePlugins()
{
    if (refreshButton.Clicked())
        Registry::Refresh();
    for (uint n = 0; n < cardButtons.length(); n++)
    {
        if (!cardButtons[n].Clicked())
            continue;
        string action = cardActions[n];
        int colon = action.findFirst(":");
        string verb = action.substr(0, colon);
        string target = action.substr(colon + 1);
        Log::Info(verb + " " + target);
        if (verb == "install")
            Plugins::Install(target);
        else if (verb == "remove")
            Plugins::Remove(target);
        else if (verb == "open")
            Host::OpenUrl(target);
        else if (verb == "host")
            Plugins::UpdateHost();
    }
    if (Host::Time() - lastCardCheck > 0.25)
    {
        lastCardCheck = Host::Time();
        if (CardState() != shownCards)
            BuildCards();
    }
}

// --- settings -----------------------------------------------------------------------------------------------------

string PluginName(const string &in id)
{
    int p = InstalledIndex(id);
    return p >= 0 ? Plugins::Name(p) : id;
}

// The plugins with something to show here, in load order: settings, or windows that can be dragged.
array<string> SettingsPlugins()
{
    array<string> ids;
    for (uint p = 0; p < Plugins::Count(); p++)
    {
        string id = Plugins::Id(p);
        bool any = UI::HasMovable(id);
        for (uint i = 0; i < Settings::Count() && !any; i++)
            any = Settings::Plugin(i) == id && !Settings::Hidden(i);
        if (any)
            ids.insertLast(id);
    }
    return ids;
}

string SettingsState()
{
    string state;
    array<string> ids = SettingsPlugins();
    for (uint n = 0; n < ids.length(); n++)
        state += ids[n] + "|";
    for (uint i = 0; i < Settings::Count(); i++)
        state += Settings::Plugin(i) + ":" + Settings::Name(i) + "|";
    return state;
}

void AddSettingRow(uint i)
{
    menu.NewRow();
    string label = Settings::Name(i);
    if (Settings::Description(i) != "")
        label += "\n" + Settings::Description(i);
    menu.AddText(label, 17);
    menu.AddSpace(0);
    string kind = Settings::Kind(i);
    if (kind == "bool")
    {
        toggles.insertLast(menu.AddButton(Settings::Get(i) == "true" ? "on" : "off"));
        toggleSetting.insertLast(i);
    }
    else if (kind != "string" && Settings::HasRange(i))
    {
        sliders.insertLast(menu.AddSlider(360));
        sliderSetting.insertLast(i);
    }
    else
    {
        inputs.insertLast(menu.AddTextInput(260, "type a value, Enter", 17));
        inputSetting.insertLast(i);
    }
    UI::Text@ value = menu.AddText(Settings::Get(i), 17);
    value.SetColor(0.7f, 0.7f, 0.75f, 1);
    values.insertLast(value);
    valueSetting.insertLast(i);
    resets.insertLast(menu.AddButton("reset"));
    resetSetting.insertLast(i);
}

void BuildSettings()
{
    menu.ClearView(settingsView);
    toggles.resize(0);
    toggleSetting.resize(0);
    sliders.resize(0);
    sliderSetting.resize(0);
    inputs.resize(0);
    inputSetting.resize(0);
    values.resize(0);
    valueSetting.resize(0);
    resets.resize(0);
    resetSetting.resize(0);
    positionResets.resize(0);
    positionPlugin.resize(0);

    menu.AddText("settings", 24);
    array<string> ids = SettingsPlugins();
    if (ids.length() == 0)
    {
        menu.NewRow();
        menu.AddText("No installed plugin has settings.", 17).SetColor(0.7f, 0.7f, 0.75f, 1);
    }
    for (uint n = 0; n < ids.length(); n++)
    {
        menu.NewRow();
        menu.AddText(PluginName(ids[n]), 21).SetColor(0.55f, 0.85f, 0.0f, 1);
        menu.AddSpace(0);
        if (UI::HasMovable(ids[n]))
        {
            menu.AddText("drag it on screen while the cursor shows", 16).SetColor(0.7f, 0.7f, 0.75f, 1);
            positionResets.insertLast(menu.AddButton("reset position"));
            positionPlugin.insertLast(ids[n]);
        }
        for (uint i = 0; i < Settings::Count(); i++)
            if (Settings::Plugin(i) == ids[n] && !Settings::Hidden(i))
                AddSettingRow(i);
    }
    shownSettings = SettingsState();
}

double Fraction(uint i)
{
    double range = Settings::Max(i) - Settings::Min(i);
    return range > 0 ? (parseFloat(Settings::Get(i)) - Settings::Min(i)) / range : 0;
}

void UpdateSettings()
{
    if (SettingsState() != shownSettings)
        BuildSettings();
    for (uint n = 0; n < toggles.length(); n++)
        if (toggles[n].Clicked())
            Settings::Set(toggleSetting[n], Settings::Get(toggleSetting[n]) == "true" ? "false" : "true");
    for (uint n = 0; n < sliders.length(); n++)
    {
        uint i = sliderSetting[n];
        if (sliders[n].dragging)
            Settings::Set(i, formatFloat(Settings::Min(i) + sliders[n].value * (Settings::Max(i) - Settings::Min(i)), "", 0, 3));
        else
            sliders[n].value = float(Fraction(i));
    }
    for (uint n = 0; n < inputs.length(); n++)
        if (inputs[n].Submitted() && !Settings::Set(inputSetting[n], inputs[n].text))
            Log::Warn("not a value for " + Settings::Name(inputSetting[n]) + ": " + inputs[n].text);
    for (uint n = 0; n < resets.length(); n++)
        if (resets[n].Clicked())
            Settings::Reset(resetSetting[n]);
    for (uint n = 0; n < positionResets.length(); n++)
        if (positionResets[n].Clicked())
            UI::ResetPositions(positionPlugin[n]);
    for (uint n = 0; n < values.length(); n++)
        values[n].text = Settings::Get(valueSetting[n]);
    for (uint n = 0; n < toggles.length(); n++)
        toggles[n].label = Settings::Get(toggleSetting[n]) == "true" ? "on" : "off";
}

// --- frame ----------------------------------------------------------------------------------------------------------

void UpdateMenu()
{
    if (closeButton.Clicked())
    {
        CloseMenu();
        return;
    }
    if (folderButton.Clicked())
        Plugins::OpenFolder();
    if (consoleNav.Clicked())
        ShowView(consoleView);
    if (pluginsNav.Clicked())
        ShowView(pluginsView);
    if (settingsNav.Clicked())
        ShowView(settingsView);
    if (shownView == consoleView)
        UpdateConsole();
    else if (shownView == pluginsView)
        UpdatePlugins();
    else
        UpdateSettings();
}

void Update(float dt)
{
    if (button.Clicked())
    {
        panel.visible = !panel.visible;
        if (panel.visible)
            Refresh();
        Log::Info("panel " + (panel.visible ? "opened" : "closed"));
    }
    if (openButton.Clicked())
        OpenMenu();
    if (panel.visible && Host::Time() - lastRefresh > 1.0)
        Refresh();
    if (menu.visible)
        UpdateMenu();
}
