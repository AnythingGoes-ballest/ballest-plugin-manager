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
UI::Button@ closeButton;
int consoleView = 0;            // the first view, the one rows go into before StartView
int pluginsView = -1;
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

void Refresh()
{
    panel.Clear();
    for (uint i = 0; i < Plugins::Count(); i++)
        panel.AddLine(Plugins::Name(i) + "   " + Plugins::Version(i) + "   " + Plugins::Status(i));
    lastRefresh = Host::Time();
}

//   plugin manager  | console                                        | plugins        3 in the registry [refresh]
//   [console]       | +------------------------------------------+   | [icon] Replay Manager  0.1.0   status [..]
//   [plugins]       | | host log                                 |   | [icon] Grind Timer     0.1.0   status [..]
//                   | +------------------------------------------+   |
//   [close]         | >  [ command                         ] [run]   |
// Widths and heights of 0 fill the space left over.
void BuildMenu()
{
    @menu = UI::CreateWindow();
    menu.SetScreenSize(0.9f, 0.9f);
    menu.SetBackground(0.02f, 0.02f, 0.03f, 0.94f);
    menu.visible = false;

    menu.StartSidebar(220);
    menu.AddText("plugin manager", 24);
    @consoleNav = menu.AddButton("console");
    @pluginsNav = menu.AddButton("plugins");
    menu.AddText(" ", 16);
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
    ShowView(consoleView);
}

void ShowView(int view)
{
    shownView = view;
    menu.ShowView(view);
    bool console = view == consoleView;
    consoleNav.SetBackground(console ? NAV_SELECTED_R : NAV_R, console ? NAV_SELECTED_G : NAV_G, console ? NAV_SELECTED_B : NAV_B, 1);
    pluginsNav.SetBackground(console ? NAV_R : NAV_SELECTED_R, console ? NAV_G : NAV_SELECTED_G, console ? NAV_B : NAV_SELECTED_B, 1);
    if (console)
        commandInput.Focus();
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
    string state = Registry::State();
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
    }
    if (Host::Time() - lastCardCheck > 0.25)
    {
        lastCardCheck = Host::Time();
        if (CardState() != shownCards)
            BuildCards();
    }
}

// --- frame ----------------------------------------------------------------------------------------------------------

void UpdateMenu()
{
    if (closeButton.Clicked())
    {
        CloseMenu();
        return;
    }
    if (consoleNav.Clicked())
        ShowView(consoleView);
    if (pluginsNav.Clicked())
        ShowView(pluginsView);
    if (shownView == consoleView)
        UpdateConsole();
    else
        UpdatePlugins();
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
