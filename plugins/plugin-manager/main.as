// Plugin Manager: a "plugins" entry in the game's footer (main menu, inside maps and in the track editor) that opens
// the plugin manager menu, and closes it again: tabs along the top, one card per plugin.
//   installed  every plugin here: status, and update / settings / turn off or on / remove / github. A plugin turned
//              off stays installed but doesn't run, also after a restart. When the registry names a newer plugin
//              manager (the host and this plugin), a card at the top offers it; the update finishes when the game
//              restarts.
//   browse     plugins in the registry that aren't installed: install / github. Installs take effect straight away.
//   console    the host log as it is written, and host commands (find, props, functions, ...; the full list is in
//              src/host/testchannel.hpp): type one and press Enter, or click run. "show" filters the log: everything,
//              only the commands typed here and their replies, the host, or one plugin.
//   Escape goes up one level: a settings page back to the installed tab, a tab out of the menu.
//   settings   opened from a plugin's card: that plugin's [Setting] variables (a slider and a text box for a number
//              with min and max, on/off for a bool, a text box otherwise, each with reset), and "reset position"
//              when it has windows that can be dragged.

UI::FooterButton@ button;

// Colours (red, green, blue). The game's widget colours are linear, so each is the linear value of the colour in
// the comment (a linear 0.05 shows as about 0.25 on screen).
const float WINDOW_R = 0.0052f, WINDOW_G = 0.0060f, WINDOW_B = 0.0086f;     // #101217 the menu
const float CARD_R = 0.0116f, CARD_G = 0.0137f, CARD_B = 0.0194f;           // #1c1f26 cards, tabs
const float BUTTON_R = 0.0232f, BUTTON_G = 0.0273f, BUTTON_B = 0.0382f;     // #2a2e37 secondary buttons
const float PRIMARY_R = 0.0782f, PRIMARY_G = 0.2051f, PRIMARY_B = 0.0048f;  // #4f7d0f install, update, the open tab
const float MUTED_R = 0.2582f, MUTED_G = 0.2747f, MUTED_B = 0.3185f;        // #8b8f99 versions, descriptions
const float GOOD_R = 0.5647f, GOOD_G = 0.9047f, GOOD_B = 0.0319f;           // #c6f432 running
const float WARN_R = 0.8879f, WARN_G = 0.5333f, WARN_B = 0.0762f;           // #f2c14e an update
const float BAD_R = 1.0f, BAD_G = 0.1714f, BAD_B = 0.147f;                  // #ff736b stopped, errors
const float ICON_SIZE = 52;
const int DESCRIPTION_LENGTH = 110;

UI::Window@ menu;
UI::Button@ installedTab;
UI::Button@ browseTab;
UI::Button@ consoleTab;
UI::Button@ folderButton;
UI::Button@ closeButton;
int installedView = -1;
int browseView = -1;
int consoleView = -1;
int settingsView = -1;
int shownView = -1;

// console
UI::TextArea@ logView;
UI::Dropdown@ logFilter;
array<string> filterSources;    // per option: "" everything, "commands", "host", or a plugin id
string shownFilterPlugins;      // the plugins the filter options were made from
UI::TextInput@ commandInput;
UI::Button@ runButton;
uint shownLogLines = 0;
const uint LOG_LINES_SHOWN = 400;

// installed and browse: the buttons on the cards and what each does
UI::Button@ refreshButton;
array<UI::Button@> cardButtons;
array<string> cardActions;      // "install:<id>", "remove:<id>", "settings:<id>", "on:<id>", "off:<id>", "open:<url>"
                                // or "host:<version>"
string shownCards;              // what the cards were built from; rebuilt when it changes
double lastCardCheck = 0;

// settings: the plugin shown, every control, and the setting (its index in Settings::) each one edits
string settingsPlugin;
UI::Button@ backButton;
array<UI::Button@> toggles;
array<uint> toggleSetting;
array<UI::Slider@> sliders;
array<uint> sliderSetting;
array<UI::TextInput@> inputs;
array<uint> inputSetting;
array<string> inputShown;       // per text box: the value it was last given
array<UI::Button@> resets;
array<uint> resetSetting;
UI::Button@ positionReset;
string shownSettings;           // what the settings view was built from

void Main()
{
    Log::Info("plugin manager started on host " + Host::Version());
    @button = UI::AddFooterButton("plugins");
    BuildMenu();
}

// A newer plugin manager in the registry than the one running, or "".
string HostUpdate()
{
    string latest = Registry::HostVersion();
    return latest != "" && CompareVersions(latest, Host::Version()) > 0 ? latest : "";
}

// --- the menu -------------------------------------------------------------------------------------------------------

//   plugins   [installed] [browse] [console]                               [open plugins folder] [close]
//   +--------------------------------------------------------------------------------------------------+
//   | [icon]  Grind Timer   0.2.1   update 0.3.0             [update] [settings] [remove] [github]      |
//   |         A green Trackmania-style timer of total time racing, with restarts from the beginning.   |
//   +--------------------------------------------------------------------------------------------------+
void BuildMenu()
{
    @menu = UI::CreateWindow();
    menu.SetScreenSize(0.8f, 0.85f);
    menu.SetBackground(WINDOW_R, WINDOW_G, WINDOW_B, 0.97f);
    menu.SetCardBackground(CARD_R, CARD_G, CARD_B, 1);
    menu.SetBlocksClicks(true);         // the game's menu underneath must not get clicks through it
    menu.zOrder = 500;                  // in front of every other plugin's windows (they default to 100)
    menu.visible = false;

    menu.StartHeader();
    menu.AddText("plugins", 28);
    menu.AddSpace(20);
    @installedTab = menu.AddButton("installed");
    @browseTab = menu.AddButton("browse");
    @consoleTab = menu.AddButton("console");
    menu.AddSpace(0);
    @folderButton = menu.AddButton("open plugins folder");
    @closeButton = menu.AddButton("close");
    Secondary(folderButton);
    Secondary(closeButton);

    installedView = menu.StartView();
    browseView = menu.StartView();
    BuildCards();
    consoleView = menu.StartView();
    BuildConsole();
    settingsView = menu.StartView();
    // Long lists scroll; the console's log already fills its view and scrolls itself.
    menu.SetScrolling(installedView, true);
    menu.SetScrolling(browseView, true);
    menu.SetScrolling(settingsView, true);
    ShowView(installedView);
}

void Secondary(UI::Button@ b) { b.SetBackground(BUTTON_R, BUTTON_G, BUTTON_B, 1); }
void Primary(UI::Button@ b) { b.SetBackground(PRIMARY_R, PRIMARY_G, PRIMARY_B, 1); }

void Tab(UI::Button@ b, bool open)
{
    if (open)
        Primary(b);
    else
        b.SetBackground(CARD_R, CARD_G, CARD_B, 1);
}

void ShowView(int view)
{
    shownView = view;
    menu.ShowView(view);
    Tab(installedTab, view == installedView || view == settingsView);
    Tab(browseTab, view == browseView);
    Tab(consoleTab, view == consoleView);
    if (view == consoleView)
        commandInput.Focus();
}

void OpenMenu()
{
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

UI::Text@ Muted(UI::Text@ t)
{
    t.SetColor(MUTED_R, MUTED_G, MUTED_B, 1);
    return t;
}

// --- console ----------------------------------------------------------------------------------------------------

void BuildConsole()
{
    menu.ClearView(consoleView);
    Muted(menu.AddText("show", 17));
    @logFilter = menu.AddDropdown(260);
    filterSources.resize(0);
    AddFilter("everything", "");
    AddFilter("my commands", "commands");
    AddFilter("host", "host");
    for (uint i = 0; i < Plugins::Count(); i++)
        AddFilter(Plugins::Name(i), Plugins::Id(i));
    logFilter.selected = 0;
    shownFilterPlugins = FilterPlugins();
    menu.NewRow();
    menu.StartCard();
    @logView = menu.AddTextArea(0, 0, 16);
    menu.EndCard();
    menu.StartCard();
    Muted(menu.AddText(">", 20));
    @commandInput = menu.AddTextInput(0, "find PlayerController, props <Class>, functions <Class>, ...", 18);
    @runButton = menu.AddButton("run");
    Secondary(runButton);
    menu.EndCard();
}

void AddFilter(const string &in label, const string &in source)
{
    logFilter.AddOption(label);
    filterSources.insertLast(source);
}

string FilterPlugins()
{
    string ids;
    for (uint i = 0; i < Plugins::Count(); i++)
        ids += Plugins::Id(i) + "|";
    return ids;
}

// A log line is "[time] [level] [source] message": where the source's "] " ends, or -1.
int SourceEnd(const string &in line)
{
    int a = line.findFirst("] [");
    int b = a < 0 ? -1 : line.findFirst("] [", a + 3);
    return b < 0 ? -1 : line.findFirst("] ", b + 3);
}

string SourceOf(const string &in line)
{
    int a = line.findFirst("] [");
    int b = a < 0 ? -1 : line.findFirst("] [", a + 3);
    int c = SourceEnd(line);
    return c < 0 ? "" : line.substr(b + 3, c - b - 3);
}

string MessageOf(const string &in line)
{
    int c = SourceEnd(line);
    return c < 0 ? line : line.substr(c + 2);
}

// "" everything, "commands" what was typed here and the host's replies, else one source (a plugin id or "host").
bool Shows(const string &in line, const string &in filter)
{
    if (filter == "")
        return true;
    string source = SourceOf(line);
    if (filter == "commands")
        return (source == "plugin-manager" && MessageOf(line).findFirst("> ") == 0) ||
               (source == "host" && MessageOf(line).findFirst("test: ") == 0);
    return source == filter;
}

// The last LOG_LINES_SHOWN lines of the host log that pass the filter, oldest first.
void ShowLog()
{
    string filter = filterSources[uint(logFilter.selected)];
    uint count = Log::LineCount();
    array<string> lines;
    for (uint i = count; i > 0 && lines.length() < LOG_LINES_SHOWN; i--)
    {
        string line = Log::Line(i - 1);
        if (line == "")
            break;                  // older lines are no longer kept
        if (Shows(line, filter))
            lines.insertLast(line);
    }
    string text;
    for (uint n = lines.length(); n > 0; n--)
    {
        text += lines[n - 1];
        if (n > 1)
            text += "\n";
    }
    logView.text = text == "" ? "Nothing yet." : text;
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
    if (FilterPlugins() != shownFilterPlugins)
    {
        BuildConsole();             // a plugin was installed or removed: new filter options
        shownLogLines = 0;
    }
    if (logFilter.Changed())
        shownLogLines = 0;
    if (Log::LineCount() != shownLogLines)
        ShowLog();
}

// --- installed and browse -------------------------------------------------------------------------------------------

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

// Everything the cards show, as one string: when it changes, the cards are rebuilt.
string CardState()
{
    string state = Registry::State() + "|host:" + HostUpdate() + ":" + Plugins::HostUpdateState();
    for (uint i = 0; i < Registry::Count(); i++)
        state += "|r:" + Registry::Id(i) + ":" + Registry::Version(i) + ":" + Registry::Icon(i) + ":" + Plugins::Pending(Registry::Id(i));
    for (uint i = 0; i < Plugins::Count(); i++)
        state += "|p:" + Plugins::Id(i) + ":" + Plugins::Version(i) + ":" + Plugins::Status(i) + ":" + Plugins::Pending(Plugins::Id(i)) +
                 (Plugins::Enabled(i) ? "" : ":off");
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

UI::Button@ AddCardButton(const string &in label, const string &in action)
{
    UI::Button@ b = menu.AddButton(label);
    Secondary(b);
    cardButtons.insertLast(b);
    cardActions.insertLast(action);
    return b;
}

// Whether a plugin has anything for its settings page: settings, or windows that can be dragged.
bool HasSettings(const string &in id)
{
    if (UI::HasMovable(id))
        return true;
    for (uint i = 0; i < Settings::Count(); i++)
        if (Settings::Plugin(i) == id && !Settings::Hidden(i))
            return true;
    return false;
}

//   [icon]  Name   version   status                              [buttons]
//           description
void AddCardTop(const string &in icon, const string &in name, const string &in version)
{
    menu.StartCard();
    menu.AddImage(icon == "" ? Plugins::DefaultIcon() : icon, ICON_SIZE, ICON_SIZE);
    menu.AddText(name, 21);
    Muted(menu.AddText(version, 16));
}

void AddCardDescription(const string &in description)
{
    menu.NewRow();
    menu.AddSpace(ICON_SIZE);
    Muted(menu.AddText(Shorten(description == "" ? "No description." : description, DESCRIPTION_LENGTH), 16));
    menu.EndCard();
}

void AddInstalledCard(uint p)
{
    string id = Plugins::Id(p);
    int r = RegistryIndex(id);
    bool newer = r >= 0 && CompareVersions(Registry::Version(r), Plugins::Version(p)) > 0;
    string pending = Plugins::Pending(id);
    string status = Plugins::Status(p);

    AddCardTop(Plugins::Icon(p), Plugins::Name(p), Plugins::Version(p));
    UI::Text@ state;
    if (pending != "")
    {
        @state = menu.AddText(pending, 16);
        if (pending.findFirst("error") == 0)
            state.SetColor(BAD_R, BAD_G, BAD_B, 1);
        else
            Muted(state);
    }
    else if (!Plugins::Enabled(p))
        @state = Muted(menu.AddText("off", 16));
    else if (status != "running")
    {
        @state = menu.AddText(status, 16);
        state.SetColor(BAD_R, BAD_G, BAD_B, 1);
    }
    else if (newer)
    {
        @state = menu.AddText("update " + Registry::Version(r), 16);
        state.SetColor(WARN_R, WARN_G, WARN_B, 1);
    }
    else
    {
        @state = menu.AddText(Plugins::Essential(p) ? "running  (built in)" : "running", 16);
        state.SetColor(GOOD_R, GOOD_G, GOOD_B, 1);
    }
    menu.AddSpace(0);
    bool busy = pending == "installing" || pending == "removing";
    if (!busy && newer)
        Primary(AddCardButton("update", "install:" + id));
    if (HasSettings(id))
        AddCardButton("settings", "settings:" + id);
    if (!busy && !Plugins::Essential(p))
    {
        if (Plugins::Enabled(p))
            AddCardButton("turn off", "off:" + id);
        else
            Primary(AddCardButton("turn on", "on:" + id));
        AddCardButton("remove", "remove:" + id);
    }
    if (r >= 0)
        AddCardButton("github", "open:" + Registry::Page(r));
    AddCardDescription(r >= 0 ? Registry::Description(r) : Plugins::Description(p));
}

void AddBrowseCard(uint r)
{
    string id = Registry::Id(r);
    string pending = Plugins::Pending(id);
    AddCardTop(Registry::Icon(r), Registry::Name(r), Registry::Version(r));
    Muted(menu.AddText("by " + Registry::Author(r), 16));
    if (pending != "")
    {
        UI::Text@ state = menu.AddText(pending, 16);
        if (pending.findFirst("error") == 0)
            state.SetColor(BAD_R, BAD_G, BAD_B, 1);
        else
            Muted(state);
    }
    menu.AddSpace(0);
    if (pending != "installing")
        Primary(AddCardButton("install", "install:" + id));
    AddCardButton("github", "open:" + Registry::Page(r));
    AddCardDescription(Registry::Description(r));
}

// The plugin manager itself (the host and this plugin), when the registry has a newer one.
void AddHostUpdateCard()
{
    string latest = HostUpdate();
    if (latest == "")
        return;
    string state = Plugins::HostUpdateState();
    menu.StartCard();
    int self = InstalledIndex("plugin-manager");
    menu.AddImage(self >= 0 && Plugins::Icon(uint(self)) != "" ? Plugins::Icon(uint(self)) : Plugins::DefaultIcon(), ICON_SIZE, ICON_SIZE);
    string text;
    if (state == "restart")
        text = "Plugin manager " + latest + " is installed: restart the game to finish.";
    else if (state == "downloading")
        text = "Updating the plugin manager to " + latest + "...";
    else
        text = "Plugin manager " + latest + " is available (you have " + Host::Version() + ").";
    UI::Text@ line = menu.AddText(text + (state.findFirst("error") == 0 ? "\n" + state : ""), 19);
    if (state.findFirst("error") == 0)
        line.SetColor(BAD_R, BAD_G, BAD_B, 1);
    else
        line.SetColor(WARN_R, WARN_G, WARN_B, 1);
    menu.AddSpace(0);
    if (state != "restart" && state != "downloading")
        Primary(AddCardButton("update plugin manager", "host:" + latest));
    menu.EndCard();
}

void BuildCards()
{
    cardButtons.resize(0);
    cardActions.resize(0);

    menu.ClearView(installedView);
    AddHostUpdateCard();
    for (uint p = 0; p < Plugins::Count(); p++)
        AddInstalledCard(p);

    menu.ClearView(browseView);
    string state = Registry::State();
    Muted(menu.AddText(state == "ready" ? Registry::Count() + " in the registry" : "registry: " + (state == "" ? "not loaded" : state), 17));
    menu.AddSpace(0);
    @refreshButton = menu.AddButton("refresh");
    Secondary(refreshButton);
    uint shown = 0;
    for (uint r = 0; r < Registry::Count(); r++)
        if (InstalledIndex(Registry::Id(r)) < 0)
        {
            AddBrowseCard(r);
            shown++;
        }
    if (state == "ready" && shown == 0)
    {
        menu.NewRow();
        Muted(menu.AddText("Every plugin in the registry is installed.", 17));
    }
    shownCards = CardState();
}

void UpdateCards()
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
        else if (verb == "off" || verb == "on")
            Plugins::SetEnabled(target, verb == "on");
        else if (verb == "open")
            Host::OpenUrl(target);
        else if (verb == "host")
            Plugins::UpdateHost();
        else if (verb == "settings")
            OpenSettings(target);
    }
    if (Host::Time() - lastCardCheck > 0.25)
    {
        lastCardCheck = Host::Time();
        if (CardState() != shownCards)
            BuildCards();
    }
}

// --- one plugin's settings --------------------------------------------------------------------------------------

void OpenSettings(const string &in id)
{
    settingsPlugin = id;
    BuildSettings();
    ShowView(settingsView);
}

string SettingsState()
{
    string state = settingsPlugin + "|" + (UI::HasMovable(settingsPlugin) ? "movable" : "");
    for (uint i = 0; i < Settings::Count(); i++)
        if (Settings::Plugin(i) == settingsPlugin)
            state += "|" + Settings::Name(i);
    return state;
}

void AddSettingRow(uint i)
{
    menu.StartCard();
    menu.AddText(Settings::Name(i), 19);
    menu.AddSpace(0);
    string kind = Settings::Kind(i);
    if (kind == "bool")
    {
        UI::Button@ toggle = menu.AddButton(Settings::Get(i) == "true" ? "on" : "off");
        toggles.insertLast(toggle);
        toggleSetting.insertLast(i);
    }
    else
    {
        // The box shows the value: edit it and press Enter. With a range there's a slider too, and a typed number
        // is clamped to the range.
        if (kind != "string" && Settings::HasRange(i))
        {
            sliders.insertLast(menu.AddSlider(300));
            sliderSetting.insertLast(i);
        }
        UI::TextInput@ box = menu.AddTextInput(kind == "string" ? 240 : 100, "", 17);
        box.clearOnSubmit = false;
        box.value = Settings::Get(i);
        inputs.insertLast(box);
        inputSetting.insertLast(i);
        inputShown.insertLast(Settings::Get(i));
    }
    UI::Button@ reset = menu.AddButton("reset");
    Secondary(reset);
    resets.insertLast(reset);
    resetSetting.insertLast(i);
    if (Settings::Description(i) != "")
    {
        menu.NewRow();
        Muted(menu.AddText(Settings::Description(i), 16));
    }
    menu.EndCard();
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
    inputShown.resize(0);
    resets.resize(0);
    resetSetting.resize(0);
    @positionReset = null;

    int p = InstalledIndex(settingsPlugin);
    @backButton = menu.AddButton("< back");
    Secondary(backButton);
    menu.AddSpace(12);
    if (p >= 0)
        menu.AddImage(Plugins::Icon(uint(p)) == "" ? Plugins::DefaultIcon() : Plugins::Icon(uint(p)), 36, 36);
    menu.AddText((p >= 0 ? Plugins::Name(uint(p)) : settingsPlugin) + " settings", 24);

    if (UI::HasMovable(settingsPlugin))
    {
        menu.StartCard();
        menu.AddText("Window position", 19);
        menu.AddSpace(0);
        @positionReset = menu.AddButton("reset position");
        Secondary(positionReset);
        menu.NewRow();
        Muted(menu.AddText("Drag its windows anywhere on screen while the cursor shows.", 16));
        menu.EndCard();
    }
    uint shown = 0;
    for (uint i = 0; i < Settings::Count(); i++)
        if (Settings::Plugin(i) == settingsPlugin && !Settings::Hidden(i))
        {
            AddSettingRow(i);
            shown++;
        }
    if (shown == 0 && positionReset is null)
    {
        menu.NewRow();
        Muted(menu.AddText("This plugin has no settings.", 17));
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
    if (backButton.Clicked())
    {
        ShowView(installedView);
        return;
    }
    if (InstalledIndex(settingsPlugin) < 0)
    {
        ShowView(installedView);            // the plugin was removed
        return;
    }
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
    {
        bool submitted = inputs[n].Submitted();
        if (submitted && !Settings::Set(inputSetting[n], inputs[n].text))
            Log::Warn("not a value for " + Settings::Name(inputSetting[n]) + ": " + inputs[n].text);
        // The box follows the value (the slider, reset, clamping) unless the player is typing in it; after Enter it
        // shows what was kept, so a clamped or refused entry is corrected in place.
        string value = Settings::Get(inputSetting[n]);
        if (submitted || (!inputs[n].focused && value != inputShown[n]))
        {
            inputs[n].value = value;
            inputShown[n] = value;
        }
    }
    for (uint n = 0; n < resets.length(); n++)
        if (resets[n].Clicked())
            Settings::Reset(resetSetting[n]);
    if (positionReset !is null && positionReset.Clicked())
        UI::ResetPositions(settingsPlugin);
    for (uint n = 0; n < toggles.length(); n++)
    {
        bool on = Settings::Get(toggleSetting[n]) == "true";
        toggles[n].label = on ? "on" : "off";
        if (on)
            Primary(toggles[n]);
        else
            Secondary(toggles[n]);
    }
}

// --- frame ----------------------------------------------------------------------------------------------------------

void UpdateMenu()
{
    // Escape goes up one level: from a plugin's settings page back to the installed tab, from a tab out of the menu.
    if (Input::Pressed(Input::Escape))
    {
        if (shownView == settingsView)
            ShowView(installedView);
        else
            CloseMenu();
        return;
    }
    if (closeButton.Clicked())
    {
        CloseMenu();
        return;
    }
    if (folderButton.Clicked())
        Plugins::OpenFolder();
    if (installedTab.Clicked())
        ShowView(installedView);
    if (browseTab.Clicked())
        ShowView(browseView);
    if (consoleTab.Clicked())
        ShowView(consoleView);
    if (shownView == consoleView)
        UpdateConsole();
    else if (shownView == settingsView)
        UpdateSettings();
    else
        UpdateCards();
}

void Update(float dt)
{
    // The footer button opens the menu, and closes it while it's open.
    if (button.Clicked())
    {
        if (menu.visible)
            CloseMenu();
        else
            OpenMenu();
        return;
    }
    if (menu.visible)
        UpdateMenu();
}
