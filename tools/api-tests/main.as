// API tests: one or more tests for every function and property of the script API, run in the game by
// tools/api_tests.py, which installs this plugin for the run only.
//
// The plugin drives the game itself (Console::Run gives it the host's test commands) through phases, each a place in
// the game some APIs need: the main menu, the Customize page, a track before and during a race, a replay, the track
// editor and a test run in it. In each it runs that phase's tests one after another. A test is a function called
// every frame until it answers: PASS, a reason it failed, or "SKIP: why". Each answer is logged as
//   RESULT PASS|FAIL|SKIP <test> covers=<API keys> | <detail>
// and "DONE" at the end. The runner maps the keys to the documented API, so a function no test covers shows up too.
//
// Tests only use what a plugin sees. Where a function has an effect a plugin can't read back, the test reads the host's
// log (Log::Line) or asks the host's test commands for the game's state.

[Setting name="Phases" description="The phases to run, by name, comma separated ('all' for every one)"]
string Phases = "all";

[Setting name="Track" description="The level of the official track for the track and race phases ('' for the first)"]
string Track = "";

[Setting name="Editor map" description="Part of the name of a saved map to open in the editor (opened, never saved)"]
string EditorMap = "";

[Setting name="Test switch" description="Used by the Settings tests"]
bool TestSwitch = false;

[Setting name="Test number" min=0 max=10 description="Used by the Settings tests"]
int TestNumber = 3;

[Setting name="Test text" description="Used by the Settings tests"]
string TestText = "hello";

[Setting name="Test choice" choices="Red|Green|Blue" description="Used by the Settings tests"]
string TestChoice = "Green";

const string PASS = "ok";
const string WAIT = "";
const string ME = "api-tests";

funcdef string TestFn();
funcdef bool PhaseFn();

class Test
{
    string phase, name, covers;
    TestFn@ fn;
    double timeout;
}

class Phase
{
    string name;
    PhaseFn@ enter;             // called every frame until it answers true: the game is where the phase's tests run
    double timeout;
}

array<Test@> tests;
array<Phase@> phases;
int phaseIndex = -1, testIndex = -1;
bool entering = false, done = false;
double phaseStarted = 0, startedAt = -1;
int passed = 0, failed = 0, skipped = 0;
int settingsChanged = 0;

// Per test: cleared before each one starts.
int step = 0;
double t0 = 0;
uint mark = 0;                  // the log's line count when the test started
int id1 = 0, id2 = 0, id3 = 0;
double d1 = 0, d2 = 0, d3 = 0;
string s1 = "";
// Kept between tests.
UI::Window@ win;
UI::Window@ clearWin;
UI::TextInput@ clearInput;
string level = "";              // the first official track
string savedBall = "";

void Add(const string &in phase, const string &in name, const string &in covers, TestFn@ fn, double timeout = 20)
{
    Test t;
    t.phase = phase;
    t.name = name;
    t.covers = covers;
    @t.fn = fn;
    t.timeout = timeout;
    tests.insertLast(t);
}

void AddPhase(const string &in name, PhaseFn@ enter, double timeout = 90)
{
    Phase p;
    p.name = name;
    @p.enter = enter;
    p.timeout = timeout;
    phases.insertLast(p);
}

// --- helpers ------------------------------------------------------------------------------------------------------------
double Elapsed() { return Host::Time() - t0; }

bool LogSince(const string &in fragment)
{
    for (uint i = mark; i < Log::LineCount(); i++)
        if (Log::Line(i).findFirst(fragment) >= 0)
            return true;
    return false;
}

string LogLineSince(const string &in fragment)
{
    for (uint i = mark; i < Log::LineCount(); i++)
        if (Log::Line(i).findFirst(fragment) >= 0)
            return Log::Line(i);
    return "";
}

double Abs(double v) { return v < 0 ? -v : v; }

string Near(double got, double want, const string &in what, double tolerance = 1e-6)
{
    return Abs(got - want) <= tolerance ? PASS : what + ": got " + got + ", want " + want;
}

string Is(bool ok, const string &in why)
{
    return ok ? PASS : why;
}

// Every failure of a list of checks (all of them were run), or PASS. Each names the API it is about, so the runner
// fails only those and counts the test's other APIs as checked.
string All(const array<string> &in checks)
{
    string failures = "";
    for (uint i = 0; i < checks.length(); i++)
        if (checks[i] != PASS)
            failures += (failures == "" ? "" : "; ") + checks[i];
    return failures == "" ? PASS : "checks: " + failures;
}

int MyPlugin()
{
    for (uint i = 0; i < Plugins::Count(); i++)
        if (Plugins::Id(i) == ME)
            return int(i);
    return -1;
}

int MySetting(const string &in name)
{
    for (uint i = 0; i < Settings::Count(); i++)
        if (Settings::Plugin(i) == ME && Settings::Name(i) == name)
            return int(i);
    return -1;
}

bool InMenu() { return !Race::OnTrack() && !Editor::IsOpen() && !Replay::IsActive(); }

// The host's UI status line (test command "state"), once it has been logged: what is really on screen.
string ScreenState()
{
    string line = LogLineSince("test: state");
    return line;
}

// --- the runner -------------------------------------------------------------------------------------------------------
bool Wanted(const string &in phase)
{
    if (Phases == "all" || Phases == "")
        return true;
    array<string>@ list = Phases.split(",");
    for (uint i = 0; i < list.length(); i++)
        if (list[i] == phase)
            return true;
    return false;
}

void Report(Test@ t, const string &in result)
{
    string kind = result == PASS ? "PASS" : result.findFirst("SKIP:") == 0 ? "SKIP" : "FAIL";
    if (kind == "PASS") passed++;
    else if (kind == "SKIP") skipped++;
    else failed++;
    string detail = result == PASS ? "" : result;
    Log::Info("RESULT " + kind + " " + t.name + " covers=" + t.covers + " | " + detail);
}

void NextPhase()
{
    while (true)
    {
        phaseIndex++;
        if (phaseIndex >= int(phases.length()))
        {
            done = true;
            Log::Info("DONE passed=" + passed + " failed=" + failed + " skipped=" + skipped);
            return;
        }
        if (Wanted(phases[phaseIndex].name))
            break;
        for (uint i = 0; i < tests.length(); i++)
            if (tests[i].phase == phases[phaseIndex].name)
                Report(tests[i], "SKIP: phase " + phases[phaseIndex].name + " not run");
    }
    Log::Info("PHASE " + phases[phaseIndex].name);
    entering = true;
    phaseStarted = Host::Time();
    ResetScratch();
}

void ResetScratch()
{
    step = 0;
    t0 = Host::Time();
    mark = Log::LineCount();
    id1 = id2 = id3 = 0;
    d1 = d2 = d3 = 0;
    s1 = "";
}

// The next test of this phase after testIndex, or -1.
int NextTestOf(const string &in phase)
{
    for (int i = testIndex + 1; i < int(tests.length()); i++)
        if (tests[i].phase == phase)
            return i;
    return -1;
}

void Main()
{
    Register();
    startedAt = Host::Time();
    Log::Info("api tests ready: " + tests.length() + " tests in " + phases.length() + " phases");
}

void OnSettingsChanged()
{
    settingsChanged++;
}

void Update(float dt)
{
    if (done)
        return;
    if (phaseIndex < 0)
    {
        if (Host::Time() - startedAt < 8)            // the menu settles (the game's loading screen covers it)
            return;
        NextPhase();
        return;
    }
    Phase@ p = phases[phaseIndex];
    if (entering)
    {
        if (p.enter())
        {
            entering = false;
            testIndex = -1;
            testIndex = NextTestOf(p.name);
            ResetScratch();
            if (testIndex < 0)
                NextPhase();
            return;
        }
        if (Host::Time() - phaseStarted > p.timeout)
        {
            Log::Warn("could not get to phase " + p.name);
            testIndex = -1;
            for (int i = NextTestOf(p.name); i >= 0; )
            {
                Report(tests[i], "couldn't get to phase " + p.name);
                testIndex = i;
                i = NextTestOf(p.name);
            }
            testIndex = -1;
            NextPhase();
        }
        return;
    }
    Test@ t = tests[testIndex];
    string result = t.fn();
    if (result == WAIT && Host::Time() - t0 > t.timeout)
        result = "timed out after " + int(t.timeout) + " s (step " + step + ")";
    if (result == WAIT)
        return;
    Report(t, result);
    testIndex = NextTestOf(p.name);
    ResetScratch();
    if (testIndex < 0)
        NextPhase();
}

// --- phases ---------------------------------------------------------------------------------------------------------------
int pstep = 0;
double pt = 0;

void Register()
{
    // The main menu: everything that needs no track.
    AddPhase("core", function() { return InMenu(); });
    AddPhase("ui", function() { return InMenu(); });
    AddPhase("customize", function() {
        if (pstep == 0)
        {
            Console::Run("call WBP_MainMenu_UIManager_C DoCustomize Transient");
            pt = Host::Time();
            pstep = 1;
        }
        return Host::Time() - pt > 4;
    });
    AddPhase("tracks", function() { pstep = 0; return true; });
    AddPhase("replay", function() {
        if (pstep == 0)
        {
            Console::Run("fakereplay on 30");
            pstep = 1;
        }
        return Replay::IsActive();
    });
    // A track, before the race starts: opened with Tracks::Open (tested by getting here).
    AddPhase("track", function() {
        if (pstep != 10)
        {
            Console::Run("fakereplay off");
            array<string>@ levels = Tracks::Official();
            if (levels.length() == 0)
                return false;
            // Leth Trial 01 has checkpoints besides the finish (so its runs have splits); else the first track.
            level = Track != "" ? Track : levels.find("Map_LethTrial_01") >= 0 ? "Map_LethTrial_01" : levels[0];
            Tracks::Open(level);
            pstep = 10;
            pt = Host::Time();
        }
        return Race::OnTrack() && Race::TrackKey() != "" && Host::Time() - pt > 8;
    });
    // The race: started as the player's Enter on "play" does (a key posted to the game's window).
    AddPhase("race", function() {
        if (Race::IsActive())
            return true;
        if (Host::Time() - pt > 3)
        {
            Console::Run("post 13");
            pt = Host::Time();
        }
        return false;
    }, 40);
    // A workshop track, found by search and opened with OpenWorkshop.
    AddPhase("workshop", function() {
        if (pstep != 20)
        {
            pstep = 20;
            Tracks::Search("sky");
            pt = Host::Time();
        }
        if (pstep == 20 && Tracks::SearchState() == "done" && Tracks::ResultCount() > 0 && s1 == "")
        {
            s1 = Tracks::ResultId(0);
            Tracks::OpenWorkshop(s1);
        }
        return Race::OnTrack() && Race::IsCustomTrack() && Race::TrackKey() != "";
    }, 120);
    // The track editor, with a saved map (opened from the Create page, never saved).
    AddPhase("editor", function() {
        if (pstep != 30)
        {
            pstep = 30;
            pt = Host::Time();
            Console::Run("open Map_MainMenu");
        }
        if (Host::Time() - pt > 12 && Host::Time() - pt < 13)
            Console::Run("openmap " + EditorMap);
        return Editor::IsOpen() && Editor::Pieces().length() > 1 && Host::Time() - pt > 20;
    }, 120);
    // A test run in the editor.
    AddPhase("editortest", function() {
        if (pstep != 40)
        {
            pstep = 40;
            Console::Run("callx W_MapEditor_C BndEvt__W_MapEditor_WBP_PlayFromStartButton_K2Node_ComponentBoundEvent_2_ButtonPressed__DelegateSignature BallgameGameInstance | i:0");
        }
        return Editor::IsTesting();
    }, 40);

    RegisterCore();
    RegisterUi();
    RegisterCustomize();
    RegisterTracks();
    RegisterReplay();
    RegisterTrack();
    RegisterRace();
    RegisterWorkshop();
    RegisterEditor();
}

// --- core: Math, Log, Host, Plugins, Settings, Registry, Console, Storage, Input -----------------------------------------
void RegisterCore()
{
    Add("core", "Math sin cos tan", "Math::sin,Math::cos,Math::tan", function() {
        array<string> c = {Near(Math::sin(Math::atan(1) * 2), 1, "Math::sin(pi/2)"), Near(Math::cos(0), 1, "Math::cos(0)"), Near(Math::tan(Math::atan(1)), 1, "Math::tan(pi/4)")};
        return All(c);
    });
    Add("core", "Math asin acos atan atan2", "Math::asin,Math::acos,Math::atan,Math::atan2", function() {
        double pi = 3.141592653589793;
        array<string> c = {Near(Math::asin(1), pi / 2, "Math::asin(1)"), Near(Math::acos(1), 0, "Math::acos(1)"), Near(Math::atan(1), pi / 4, "Math::atan(1)"),
                           Near(Math::atan2(1, -1), 3 * pi / 4, "Math::atan2(1,-1)")};
        return All(c);
    });
    Add("core", "Math sqrt pow abs floor ceil", "Math::sqrt,Math::pow,Math::abs,Math::floor,Math::ceil", function() {
        array<string> c = {Near(Math::sqrt(9), 3, "Math::sqrt(9)"), Near(Math::pow(2, 10), 1024, "Math::pow(2,10)"), Near(Math::abs(-2.5), 2.5, "Math::abs(-2.5)"),
                           Near(Math::floor(-2.5), -3, "Math::floor(-2.5)"), Near(Math::ceil(2.1), 3, "Math::ceil(2.1)")};
        return All(c);
    });

    Add("core", "Log writes and reads back", "Log::Info,Log::Warn,Log::Error,Log::LineCount,Log::Line", function() {
        uint before = Log::LineCount();
        Log::Info("api-test info line");
        Log::Warn("api-test warn line");
        Log::Error("api-test error line");
        if (Log::LineCount() != before + 3)
            return "Log::LineCount went from " + before + " to " + Log::LineCount() + ", want +3";
        array<string> c = {Is(Log::Line(before).findFirst("[info] [api-tests] api-test info line") >= 0, "Log::Info line: " + Log::Line(before)),
                           Is(Log::Line(before + 1).findFirst("[warn] [api-tests] api-test warn line") >= 0, "Log::Warn line: " + Log::Line(before + 1)),
                           Is(Log::Line(before + 2).findFirst("[error] [api-tests] api-test error line") >= 0, "Log::Error line: " + Log::Line(before + 2)),
                           Is(Log::Line(999999999) == "", "Log::Line past the end is not empty")};
        return All(c);
    });

    Add("core", "Host version and time", "Host::Version,Host::Time", function() {
        if (step == 0)
        {
            d1 = Host::Time();
            step = 1;
            return WAIT;
        }
        if (Elapsed() < 0.3)
            return WAIT;
        array<string> parts = Host::Version().split(".");
        array<string> c = {Is(parts.length() == 3 && parseInt(parts[0]) >= 0 && parts[1] != "", "Host::Version '" + Host::Version() + "' is not x.y.z"),
                           Is(Host::Time() > d1 + 0.2, "Host::Time did not advance: " + d1 + " then " + Host::Time())};
        return All(c);
    });
    Add("core", "Host window", "Host::WindowMaximized,Host::WindowFitsScreen,Host::MaximizeWindow,Host::MaximizeAtStart", function() {
        // Only asked, never changed: maximizing is tested only on a window that is maximized already (a no-op).
        bool maximized = Host::WindowMaximized();
        bool fits = Host::WindowFitsScreen();
        if (maximized && !Host::MaximizeWindow())
            return "Host::MaximizeWindow on a maximized window answered false";
        Host::MaximizeAtStart(1);
        Host::MaximizeAtStart(0);            // and taken back: window_at_start.txt keeps no entry for this plugin
        Log::Info("window maximized " + maximized + ", fits the screen " + fits);
        return PASS;
    });
    Add("core", "Host::OpenUrl refuses other sites", "Host::OpenUrl", function() {
        Host::OpenUrl("https://example.com/");
        return Is(LogSince("not opening https://example.com/"), "Host::OpenUrl logged no refusal for a non-GitHub URL");
    });
    Add("core", "Host::MapNumber", "Host::MapNumber", function() {
        return Is(Host::MapNumber() >= 0, "Host::MapNumber " + Host::MapNumber());
    });

    Add("core", "Plugins lists this plugin", "Plugins::Count,Plugins::Id,Plugins::Name,Plugins::Version,Plugins::Status,Plugins::Author,Plugins::Description,Plugins::Icon,Plugins::Essential,Plugins::Enabled", function() {
        int i = MyPlugin();
        if (i < 0)
            return "Plugins::Count/Plugins::Id: this plugin is not in the list of " + Plugins::Count();
        uint u = uint(i);
        array<string> c = {Is(Plugins::Name(u) == "API tests", "Plugins::Name '" + Plugins::Name(u) + "'"),
                           Is(Plugins::Version(u) == "0.0.1", "Plugins::Version '" + Plugins::Version(u) + "'"),
                           Is(Plugins::Status(u) == "running", "Plugins::Status '" + Plugins::Status(u) + "'"),
                           Is(Plugins::Author(u) == "AnythingGoes", "Plugins::Author '" + Plugins::Author(u) + "'"),
                           Is(Plugins::Description(u).findFirst("Tests every") == 0, "Plugins::Description '" + Plugins::Description(u) + "'"),
                           Is(Plugins::Icon(u) == "" || Plugins::Icon(u).findFirst("api-tests") >= 0, "Plugins::Icon '" + Plugins::Icon(u) + "'"),
                           Is(!Plugins::Essential(u), "Plugins::Essential: this plugin is essential"),
                           Is(Plugins::Enabled(u), "Plugins::Enabled: this plugin is not enabled"),
                           Is(Plugins::Id(99999) == "", "Plugins::Id past the end is not empty")};
        bool manager = false;
        for (uint k = 0; k < Plugins::Count(); k++)
            manager = manager || (Plugins::Id(k) == "plugin-manager" && Plugins::Essential(k));
        c.insertLast(Is(manager, "Plugins::Essential: the plugin manager is not listed as essential"));
        return All(c);
    });
    Add("core", "Plugins install state", "Plugins::IsInstalled,Plugins::InstalledVersion,Plugins::Pending,Plugins::DefaultIcon,Plugins::Folder,Plugins::HostUpdateState", function() {
        string folder = Plugins::Folder();
        string hostUpdate = Plugins::HostUpdateState();
        array<string> c = {Is(Plugins::IsInstalled(ME), "Plugins::IsInstalled(self) false"), Is(!Plugins::IsInstalled("no-such-plugin"), "Plugins::IsInstalled(missing) true"),
                           Is(Plugins::InstalledVersion(ME) == "0.0.1", "Plugins::InstalledVersion '" + Plugins::InstalledVersion(ME) + "'"),
                           Is(Plugins::InstalledVersion("no-such-plugin") == "", "Plugins::InstalledVersion(missing) not empty"),
                           Is(Plugins::Pending("no-such-plugin") == "", "Plugins::Pending '" + Plugins::Pending("no-such-plugin") + "'"),
                           Is(Plugins::DefaultIcon() != "", "Plugins::DefaultIcon empty"),
                           Is(folder.findFirst("plugins\\api-tests\\") > 0, "Plugins::Folder '" + folder + "'"),
                           Is(hostUpdate == "" || hostUpdate == "downloading" || hostUpdate == "restart" || hostUpdate.findFirst("error") == 0,
                              "Plugins::HostUpdateState '" + hostUpdate + "'")};
        return All(c);
    });
    Add("core", "Plugins management is the plugin manager's", "Plugins::Install,Plugins::Remove,Plugins::SetEnabled,Plugins::UpdateHost", function() {
        Plugins::Install("no-such-plugin");
        Plugins::Remove("no-such-plugin");
        Plugins::SetEnabled("no-such-plugin", false);
        Plugins::UpdateHost();
        int refusals = 0;
        for (uint i = mark; i < Log::LineCount(); i++)
            if (Log::Line(i).findFirst("[api-tests] only the plugin manager can install or remove plugins") >= 0)
                refusals++;
        return Is(refusals == 4, refusals + " of 4 calls refused (only the plugin manager may)");
    });
    Add("core", "Plugins::OpenFolder", "Plugins::OpenFolder", function() {
        return "SKIP: opens a File Explorer window on the player's screen";
    });

    Add("core", "Settings lists this plugin's", "Settings::Count,Settings::Plugin,Settings::Name,Settings::Description,Settings::Kind,Settings::Hidden,Settings::HasRange,Settings::Min,Settings::Max,Settings::Get,Settings::IsDefault", function() {
        int b = MySetting("Test switch"), n = MySetting("Test number"), t = MySetting("Test text");
        if (b < 0 || n < 0 || t < 0)
            return "Settings::Count/Settings::Plugin/Settings::Name: own settings not found among " + Settings::Count();
        array<string> c = {Is(Settings::Plugin(uint(b)) == ME, "Settings::Plugin"), Is(Settings::Description(uint(b)) == "Used by the Settings tests", "Settings::Description '" + Settings::Description(uint(b)) + "'"),
                           Is(Settings::Kind(uint(b)) == "bool" && Settings::Kind(uint(n)) == "int" && Settings::Kind(uint(t)) == "string",
                              "Settings::Kind " + Settings::Kind(uint(b)) + "/" + Settings::Kind(uint(n)) + "/" + Settings::Kind(uint(t))),
                           Is(!Settings::Hidden(uint(b)), "Settings::Hidden"), Is(Settings::HasRange(uint(n)) && !Settings::HasRange(uint(b)), "Settings::HasRange"),
                           Near(Settings::Min(uint(n)), 0, "Settings::Min"), Near(Settings::Max(uint(n)), 10, "Settings::Max"),
                           Is(Settings::Get(uint(n)) == "3", "Settings::Get '" + Settings::Get(uint(n)) + "'"),
                           Is(Settings::IsDefault(uint(t)), "Settings::IsDefault false before any change")};
        return All(c);
    });
    Add("core", "Settings choices", "Settings::Choices", function() {
        int ch = MySetting("Test choice"), t = MySetting("Test text");
        if (ch < 0 || t < 0)
            return "own choice setting not found";
        array<string>@ options = Settings::Choices(uint(ch));
        array<string> c = {Is(options.length() == 3 && options[0] == "Red" && options[2] == "Blue", "Settings::Choices " + options.length()),
                           Is(Settings::Choices(uint(t)).length() == 0, "Settings::Choices of a setting without choices not empty"),
                           Is(!Settings::Set(uint(ch), "Purple"), "Settings::Set of a value not among the choices answered true"),
                           Is(Settings::Set(uint(ch), "Blue") && TestChoice == "Blue", "Settings::Set of a choice: '" + TestChoice + "'")};
        Settings::Reset(uint(ch));
        return All(c);
    });
    Add("core", "Settings set and reset own", "Settings::Set,Settings::Reset", function() {
        int n = MySetting("Test number");
        if (step == 0)
        {
            id1 = settingsChanged;
            if (!Settings::Set(uint(n), "7"))
                return "Settings::Set on own setting answered false";
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (TestNumber != 7 || Settings::Get(uint(n)) != "7" || Settings::IsDefault(uint(n)))
                return "Settings::Set: after it, the variable " + TestNumber + ", Get '" + Settings::Get(uint(n)) + "'";
            if (settingsChanged <= id1)
                return "Settings::Set did not call OnSettingsChanged";
            Settings::Reset(uint(n));
            step = 2;
            return WAIT;
        }
        if (TestNumber != 3 || !Settings::IsDefault(uint(n)))
            return "Settings::Reset: after it, the variable " + TestNumber;
        for (uint i = 0; i < Settings::Count(); i++)
            if (Settings::Plugin(i) != ME && Settings::Plugin(i) != "")
                return Is(!Settings::Set(i, Settings::Get(i)), "Settings::Set on another plugin's setting was allowed");
        return PASS;
    });

    Add("core", "Registry loads", "Registry::Refresh,Registry::State,Registry::Count,Registry::Id,Registry::Name,Registry::Description,Registry::Author,Registry::Version,Registry::Page,Registry::Icon,Registry::HostVersion", function() {
        if (step == 0)
        {
            Registry::Refresh();
            step = 1;
            return WAIT;
        }
        string state = Registry::State();
        if (state.findFirst("error") == 0)
            return "Registry::State " + state;
        if (state != "ready" || Registry::Count() == 0)
            return WAIT;
        array<string> c = {Is(Registry::Id(0) != "", "Registry::Id(0) empty"), Is(Registry::Name(0) != "", "Registry::Name(0) empty"),
                           Is(Registry::Description(0) != "", "Registry::Description(0) empty"), Is(Registry::Author(0) != "", "Registry::Author(0) empty"),
                           Is(Registry::Version(0).split(".").length() >= 2, "Registry::Version(0) '" + Registry::Version(0) + "'"),
                           Is(Registry::Page(0).findFirst("https://") == 0, "Registry::Page(0) '" + Registry::Page(0) + "'"),
                           Is(Registry::Icon(0) == "" || Registry::Icon(0).findFirst(".png") > 0, "Registry::Icon(0) '" + Registry::Icon(0) + "'"),
                           Is(Registry::HostVersion().split(".").length() == 3, "Registry::HostVersion '" + Registry::HostVersion() + "'"),
                           Is(Registry::Id(99999) == "", "Registry::Id past the end is not empty")};
        return All(c);
    }, 30);

    Add("core", "Console runs a host command", "Console::Run", function() {
        if (step == 0)
        {
            Console::Run("state");
            step = 1;
            return WAIT;
        }
        return LogSince("test: state") ? PASS : WAIT;
    }, 5);

    Add("core", "Storage keeps values", "Storage::Get,Storage::Set", function() {
        Storage::Set("api-test-key", "value 1");
        array<string> c = {Is(Storage::Get("api-test-key") == "value 1", "Storage::Get after Storage::Set '" + Storage::Get("api-test-key") + "'"),
                           Is(Storage::Get("no-such-key", "fallback") == "fallback", "Storage::Get of a missing key without its fallback"),
                           Is(Storage::Get("no-such-key") == "", "Storage::Get: default fallback not empty")};
        Storage::Set("api-test-key", "");
        return All(c);
    });

    Add("core", "Input keys from the host", "Input::Pressed,Input::Down,Input::AnyPressed,Input::Name", function() {
        // The host's test hooks stand in for the keyboard: "press" reports a key pressed for one frame, "hold" held.
        if (step == 0)
        {
            Console::Run("press 71");
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (!Input::Pressed(Input::Key::G))
                return Elapsed() > 2 ? "Input::Pressed(G) never true after a simulated press" : WAIT;
            if (Input::AnyPressed() != Input::Key::G)
                return "Input::AnyPressed " + Input::Name(Input::AnyPressed()) + ", want G";
            Console::Run("hold 72 1");
            step = 2;
            return WAIT;
        }
        if (step == 2)
        {
            if (!Input::Down(Input::Key::H))
                return Elapsed() > 4 ? "Input::Down(H) never true while held" : WAIT;
            Console::Run("hold 72 0");
            step = 3;
            return WAIT;
        }
        if (Input::Down(Input::Key::H))
            return Elapsed() > 6 ? "Input::Down(H) still true after release" : WAIT;
        array<string> c = {Is(Input::Name(Input::Key::G) == "G", "Input::Name(G) '" + Input::Name(Input::Key::G) + "'"),
                           Is(Input::Name(Input::Key::Space) == "Space", "Input::Name(Space)"), Is(Input::Name(Input::Key::PadA) == "PadA", "Input::Name(PadA)")};
        return All(c);
    });
    Add("core", "Input mouse and wheel", "Input::MousePosition,Input::Wheel", function() {
        // Where the real mouse is isn't up to the test: only that the answers are sane.
        float x = -1, y = -1;
        bool got = Input::MousePosition(x, y);
        float wheel = Input::Wheel();
        array<string> c = {Is(!got || (x > -100000 && x < 100000 && y > -100000 && y < 100000), "Input::MousePosition " + x + "," + y),
                           Is(wheel == wheel && wheel > -1000 && wheel < 1000, "Input::Wheel " + wheel)};
        return All(c);
    });
}

// --- UI ---------------------------------------------------------------------------------------------------------------------
UI::Text@ uText;
UI::Text@ uPlaced;
UI::Rect@ uRect;
UI::Button@ uButton;
UI::Button@ uIcon;
UI::Slider@ uSlider;
UI::Dropdown@ uDrop;
UI::TextArea@ uArea;
UI::TextInput@ uInput;
UI::Image@ uImage;
UI::CheckBox@ uCheck;
UI::FooterButton@ uFooter;
UI::Panel@ uPanel;
UI::FooterButton@ uPanelButton;
int uView = 0;

void RegisterUi()
{
    Add("ui", "UI screen and cursor", "UI::ScreenSize,UI::FooterHeight,UI::CursorShown,UI::SetCursorVisible", function() {
        if (step == 0)
        {
            float w = 0, h = 0;
            if (!UI::ScreenSize(w, h) || w < 100 || h < 100)
                return "UI::ScreenSize " + w + "x" + h;
            if (UI::FooterHeight() < 0 || UI::FooterHeight() > h / 2)
                return "UI::FooterHeight " + UI::FooterHeight();
            UI::SetCursorVisible(true);
            step = 1;
            return WAIT;
        }
        if (!UI::CursorShown())
            return Elapsed() > 3 ? "UI::CursorShown false after UI::SetCursorVisible(true)" : WAIT;
        UI::SetCursorVisible(false);
        return PASS;
    });
    Add("ui", "UI window with every widget", "UI::CreateWindow,Window.SetAnchor,Window.SetPivot,Window.SetOffset,Window.SetBackground,Window.SetCornerRadius,Window.visible,Window.AddText,Window.AddWrappedText,Window.AddButton,Window.AddIconButton,Window.AddSlider,Window.AddDropdown,Window.AddSpace,Window.NewRow,Window.AddCheckBox,Window.AddImage,Window.AddTextArea,Window.AddTextInput,Window.AddTextAt,Window.AddRect,Window.zOrder,Window.SetBlocksClicks,Dropdown.AddOption", function() {
        if (step == 0)
        {
            @win = UI::CreateWindow();
            win.SetAnchor(0.5f, 0.5f);
            win.SetPivot(0.5f, 0.5f);
            win.SetOffset(0, 0);
            win.SetBackground(0.1f, 0.1f, 0.2f, 0.9f);
            win.SetCornerRadius(8);
            win.zOrder = 150;
            win.SetBlocksClicks(true);
            @uText = win.AddText("api text one", 18);
            win.AddWrappedText(180, "api wrapped text that is deliberately long enough to wrap", 14);
            win.AddSpace(10);
            @uButton = win.AddButton("api button");
            @uIcon = win.AddIconButton("play");
            win.NewRow();
            @uSlider = win.AddSlider(200);
            @uDrop = win.AddDropdown(120);
            uDrop.AddOption("apifirst");
            uDrop.AddOption("apisecond");
            uDrop.AddOption("apithird");
            @uCheck = win.AddCheckBox("api check", 16);
            win.NewRow();
            @uImage = win.AddImage(Plugins::Folder() + "test.png", 64, 32);
            @uArea = win.AddTextArea(300, 60, 14);
            uArea.text = "api area line";
            @uInput = win.AddTextInput(200, "api hint", 16);
            @uPlaced = win.AddTextAt("api placed", 14, 10, 150);
            @uRect = win.AddRect(10, 170, 50, 8);
            win.visible = true;
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 1)
                return WAIT;
            Console::Run("state");
            step = 2;
            return WAIT;
        }
        string line = ScreenState();
        if (line == "")
            return WAIT;
        array<string> c = {Is(line.findFirst("window=shown") >= 0, "UI::CreateWindow/Window.visible: the window is not shown"),
                           Is(line.findFirst("text[api text one]") >= 0, "Window.AddText: its text is not on screen"),
                           Is(line.findFirst("text[api wrapped text") >= 0, "Window.AddWrappedText: its text is not on screen"),
                           Is(line.findFirst("text[api placed]") >= 0, "Window.AddTextAt: the placed text is not on screen"),
                           Is(win.visible && win.zOrder == 150, "Window.zOrder/Window.visible read back wrong")};
        return All(c);
    });
    Add("ui", "UI text", "Text.text,Text.SetColor,Text.size,Text.SetWidth,Text.SetAlign,Text.visible,Text.SetPosition,Rect.SetRect,Rect.SetColor,Rect.visible", function() {
        if (win is null)
            return "no window (the widget test failed)";
        if (step == 0)
        {
            uText.text = "api text two";
            uText.SetColor(1, 0.5f, 0.2f, 1);
            uText.size = 20;
            uText.SetWidth(250);
            uText.SetAlign(1);
            uPlaced.SetPosition(20, 150);
            uRect.SetRect(20, 172, 80, 8);
            uRect.SetColor(0.2f, 0.9f, 0.3f, 1);
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 1)
                return WAIT;
            mark = Log::LineCount();
            Console::Run("state");
            step = 2;
            return WAIT;
        }
        if (step == 2)
        {
            string line = ScreenState();
            if (line == "")
                return WAIT;
            if (line.findFirst("text[api text two]") < 0)
                return "Text.text: the changed text is not on screen";
            if (uText.text != "api text two" || uText.size != 20)
                return "Text.text/Text.size read back '" + uText.text + "' " + uText.size;
            uText.visible = false;
            uRect.visible = false;
            if (uText.visible || uRect.visible)
                return "Text.visible/Rect.visible read back true after hiding";
            mark = Log::LineCount();
            t0 = Host::Time();
            step = 3;
            return WAIT;
        }
        if (step == 3)
        {
            if (Elapsed() < 1)
                return WAIT;
            Console::Run("state");
            step = 4;
            return WAIT;
        }
        string after = ScreenState();
        if (after == "")
            return WAIT;
        uText.visible = true;
        uRect.visible = true;
        return Is(after.findFirst("text[api text two]") < 0, "Text.visible: the hidden text is still on screen");
    });
    Add("ui", "UI button click", "Button.Clicked,Button.hovered,Button.SetBackground,Button.label,Button.icon,Button.visible", function() {
        if (win is null)
            return "no window";
        if (step == 0)
        {
            uButton.SetBackground(0.3f, 0.3f, 0.6f, 1);
            uButton.label = "api button two";
            uIcon.icon = "pause";
            if (uButton.Clicked())
                return "Button.Clicked true before any click";
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 0.5)
                return WAIT;
            Console::Run("click api button two");
            step = 2;
            return WAIT;
        }
        if (!uButton.Clicked())
            return WAIT;
        if (uButton.Clicked())
            return "Button.Clicked stayed true (it should be taken once)";
        if (uButton.hovered)
            Log::Info("the button reads as hovered (the real mouse is over it)");
        uButton.visible = false;
        bool hidden = !uButton.visible;
        uButton.visible = true;
        return Is(hidden, "Button.visible read back true after hiding");
    }, 10);
    Add("ui", "UI slider", "Slider.value,Slider.dragging,Slider.visible", function() {
        if (win is null)
            return "no window";
        if (step == 0)
        {
            uSlider.value = 0.25f;
            if (Abs(uSlider.value - 0.25f) > 0.001)
                return "Slider.value read back " + uSlider.value;
            uSlider.value = 2;
            if (uSlider.value != 1)
                return "Slider.value above 1 kept as " + uSlider.value;
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 0.5)
                return WAIT;
            Console::Run("slider 0.6");
            step = 2;
            return WAIT;
        }
        if (Abs(uSlider.value - 0.6f) > 0.01)
            return WAIT;
        return Is(uSlider.visible, "Slider.visible false");
    }, 10);
    Add("ui", "UI dropdown", "Dropdown.selected,Dropdown.Changed,Dropdown.ClearOptions,Dropdown.visible", function() {
        if (win is null)
            return "no window";
        if (step == 0)
        {
            uDrop.selected = 1;
            if (uDrop.selected != 1)
                return "Dropdown.selected read back " + uDrop.selected;
            uDrop.selected = 9;
            if (uDrop.selected != 1)
                return "Dropdown.selected: an index past the options was taken";
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 0.5)
                return WAIT;
            Console::Run("select apifirst 2");
            step = 2;
            return WAIT;
        }
        if (step == 2)
        {
            if (!uDrop.Changed())
                return WAIT;
            if (uDrop.selected != 2)
                return "Dropdown.selected " + uDrop.selected + " after choosing the third";
            uDrop.ClearOptions();
            uDrop.AddOption("api only");
            step = 3;
            return WAIT;
        }
        return Is(uDrop.visible, "Dropdown.visible false");
    }, 10);
    Add("ui", "UI check box", "CheckBox.checked,CheckBox.Changed,CheckBox.SetColor,CheckBox.visible", function() {
        if (win is null)
            return "no window";
        if (step == 0)
        {
            uCheck.SetColor(0.9f, 0.2f, 0.2f, 1);
            uCheck.checked = false;
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 0.5)
                return WAIT;
            Console::Run("click api check");
            step = 2;
            return WAIT;
        }
        if (!uCheck.Changed())
            return WAIT;
        if (!uCheck.checked)
            return "CheckBox.checked false after a click";
        uCheck.visible = false;
        bool hidden = !uCheck.visible;
        uCheck.visible = true;
        return Is(hidden, "CheckBox.visible read back true after hiding");
    }, 10);
    Add("ui", "UI text input", "TextInput.Submitted,TextInput.text,TextInput.typed,TextInput.focused,TextInput.Focus,TextInput.Submit,TextInput.value,TextInput.clearOnSubmit,TextInput.readOnly,TextInput.visible", function() {
        if (win is null)
            return "no window";
        if (step == 0)
        {
            uInput.clearOnSubmit = false;
            uInput.readOnly = false;
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 0.5)
                return WAIT;
            Console::Run("type @api hint|typed words");
            step = 2;
            return WAIT;
        }
        if (step == 2)
        {
            if (uInput.typed != "typed words")
                return WAIT;
            Console::Run("submit @api hint|sent words");
            step = 3;
            return WAIT;
        }
        if (step == 3)
        {
            if (!uInput.Submitted())
                return WAIT;
            if (uInput.text != "sent words")
                return "TextInput.text '" + uInput.text + "' after a submit";
            uInput.value = "set words";
            uInput.Submit();
            step = 4;
            t0 = Host::Time();
            return WAIT;
        }
        if (step == 4)
        {
            if (!uInput.Submitted())
                return Elapsed() > 3 ? "TextInput.Submit after setting TextInput.value was not submitted" : WAIT;
            if (uInput.text != "set words")
                return "TextInput.text '" + uInput.text + "' after TextInput.value + TextInput.Submit";
            uInput.Focus();
            step = 5;
            t0 = Host::Time();
            return WAIT;
        }
        if (step == 5)
        {
            // Keyboard focus needs the game's window to be the active one; without it, Focus() can't take effect.
            if (!uInput.focused && Elapsed() < 2)
                return WAIT;
            bool focused = uInput.focused;
            uInput.readOnly = true;
            uInput.visible = false;
            bool hidden = !uInput.visible;
            uInput.visible = true;
            uInput.readOnly = false;
            if (!hidden)
                return "TextInput.visible read back true after hiding";
            return focused ? PASS : "SKIP: focus needs the game to be the active window (Focus() was called)";
        }
        return PASS;
    }, 12);
    Add("ui", "UI text input clear button", "TextInput.clearButton,TextInput.Cleared", function() {
        // The x is clicked by the player; here: a box with one builds, and Cleared() stays false until it's clicked.
        if (step == 0)
        {
            @clearWin = UI::CreateWindow();
            @clearInput = clearWin.AddTextInput(200, "api clear");
            clearInput.clearButton = true;
            clearInput.value = "words";
            step = 1;
            return WAIT;
        }
        if (Elapsed() < 0.5)
            return WAIT;
        bool cleared = clearInput.Cleared();
        clearWin.visible = false;
        return Is(!cleared, "TextInput.Cleared true without a click");
    }, 5);
    Add("ui", "UI text area and image", "TextArea.text,TextArea.visible,Image.path,Image.visible", function() {
        if (win is null)
            return "no window";
        uArea.text = "api area two";
        uImage.path = Plugins::Folder() + "test.png";
        array<string> c = {Is(uArea.text == "api area two", "TextArea.text '" + uArea.text + "'"),
                           Is(uImage.path.findFirst("test.png") > 0, "Image.path '" + uImage.path + "'"),
                           Is(uArea.visible && uImage.visible, "TextArea.visible/Image.visible false")};
        return All(c);
    });
    Add("ui", "UI views, cards, sidebar and scrolling", "Window.StartView,Window.ShowView,Window.ClearView,Window.SetScrolling,Window.StartSidebar,Window.StartMain,Window.StartHeader,Window.StartCard,Window.EndCard,Window.SetCardBackground", function() {
        if (step == 0)
        {
            UI::Window@ w = UI::CreateWindow();
            w.SetAnchor(0, 0);
            w.SetPivot(0, 0);
            w.SetOffset(40, 40);
            w.StartHeader();
            w.AddText("api header", 16);
            w.StartSidebar(100);
            w.AddText("api side", 14);
            w.StartMain();
            w.SetCardBackground(0.2f, 0.2f, 0.25f, 1);
            w.StartCard();
            w.AddText("api card", 14);
            w.EndCard();
            uView = w.StartView();
            w.AddText("api second view", 14);
            w.SetScrolling(uView, true);
            w.ShowView(uView);
            w.ClearView(uView);
            w.AddText("api second view", 14);        // refilled after clearing: the view shows what it has now
            id1 = uView;
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 1)
                return WAIT;
            Console::Run("state");
            step = 2;
            return WAIT;
        }
        string line = ScreenState();
        if (line == "")
            return WAIT;
        array<string> c = {Is(id1 > 0, "Window.StartView answered " + id1), Is(line.findFirst("text[api second view]") >= 0, "Window.ShowView: the shown view's text is not on screen"),
                           Is(line.findFirst("text[api card]") >= 0 && line.findFirst("text[api side]") >= 0 && line.findFirst("text[api header]") >= 0,
                              "Window.StartHeader/Window.StartSidebar/Window.StartCard: their text is not on screen")};
        return All(c);
    });
    Add("ui", "UI window placement", "Window.SetRect,Window.SetScreenSize,Window.movable,UI::HasMovable,UI::ResetPositions,Window.DockInEditorDetails", function() {
        UI::Window@ w = UI::CreateWindow();
        w.SetRect(100, 100, 300, 120);
        w.SetScreenSize(1920, 1080);
        w.AddText("api placed window", 14);
        w.movable = true;
        if (!w.movable)
            return "Window.movable read back false";
        if (!UI::HasMovable(ME))
            return "UI::HasMovable false with a movable window";
        UI::ResetPositions(ME);
        UI::Window@ docked = UI::CreateWindow();
        docked.DockInEditorDetails();
        docked.AddText("api docked", 14);
        return PASS;
    });
    Add("ui", "UI footer button and panel", "UI::AddFooterButton,FooterButton.Clicked,FooterButton.hovered,FooterButton.label,UI::CreatePanel,Panel.Clear,Panel.AddLine,Panel.title,Panel.visible,Panel.AddButton", function() {
        if (step == 0)
        {
            @uFooter = UI::AddFooterButton("api footer");
            @uPanel = UI::CreatePanel();
            uPanel.title = "api panel";
            uPanel.AddLine("a line");
            uPanel.Clear();
            uPanel.AddLine("api panel line");
            @uPanelButton = uPanel.AddButton("api panel button");
            uPanel.visible = true;
            if (!uPanel.visible)
                return "Panel.visible read back false";
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 1.5)
                return WAIT;
            if (!LogSince("footer button 'api footer' placed"))
                return Elapsed() > 6 ? "UI::AddFooterButton: the footer button was not placed" : WAIT;
            Console::Run("click api footer");
            step = 2;
            return WAIT;
        }
        if (step == 2)
        {
            if (!uFooter.Clicked())
                return WAIT;
            uFooter.label = "api footer two";
            if (uFooter.hovered)
                Log::Info("the footer button reads as hovered (the real mouse is over it)");
            Console::Run("click api panel button");
            step = 3;
            return WAIT;
        }
        if (!uPanelButton.Clicked())
            return WAIT;
        uPanel.visible = false;
        return PASS;
    }, 15);
}

// --- customize: Cosmetics ----------------------------------------------------------------------------------------------------
string wornBall = "", wornHat = "", wornBfx = "";
string wornArms;

void RegisterCustomize()
{
    Add("customize", "Cosmetics add", "Cosmetics::AddBall,Cosmetics::AddHat,Cosmetics::AddBfx,Cosmetics::Count", function() {
        int balls = Cosmetics::Count(Cosmetics::Ball), hats = Cosmetics::Count(Cosmetics::Hat), bfx = Cosmetics::Count(Cosmetics::Bfx);
        string f = Plugins::Folder();
        array<string> c = {Is(Cosmetics::AddBall("api-tests.ball", "api ball", f + "test.png", f + "test.png"), "Cosmetics::AddBall with an image answered false"),
                           Is(Cosmetics::AddBall("api-tests.model", "api model", "", "", f + "cube.obj"), "Cosmetics::AddBall with a 3D model file answered false"),
                           Is(Cosmetics::AddBall("api-tests.shapes", "api shapes", "", "", f + "shapes.txt"), "Cosmetics::AddBall with a text model answered false"),
                           Is(!Cosmetics::AddBall("api-tests.bad", "api bad", "C:/Windows/win.ini"), "Cosmetics::AddBall with a file outside the plugins folder answered true"),
                           Is(Cosmetics::AddHat("api-tests.hat", "api hat", "/Engine/BasicShapes/Cone.Cone", 0.45), "Cosmetics::AddHat answered false"),
                           Is(Cosmetics::AddBfx("api-tests.bfx", "api bfx", "/Game/Art/DataAssets/GoalExplosions/Fire1/DA_Fire1.DA_Fire1", 1.2), "Cosmetics::AddBfx answered false"),
                           Is(Cosmetics::Count(Cosmetics::Ball) == balls + 3, "Cosmetics::Count(Ball) " + balls + " -> " + Cosmetics::Count(Cosmetics::Ball) + ", want +3"),
                           Is(Cosmetics::Count(Cosmetics::Hat) == hats + 1, "Cosmetics::Count(Hat) +" + (Cosmetics::Count(Cosmetics::Hat) - hats)),
                           Is(Cosmetics::Count(Cosmetics::Bfx) == bfx + 1, "Cosmetics::Count(Bfx) +" + (Cosmetics::Count(Cosmetics::Bfx) - bfx))};
        return All(c);
    });
    Add("customize", "Cosmetics equip", "Cosmetics::Equip,Cosmetics::Equipped", function() {
        if (step == 0)
        {
            // Custom cosmetics show on the menu ball in local mode (the page opens in it); in public
            // mode it shows the game's own choice (host 0.17.0 on). The test sets the mode itself either way.
            Console::Run("cosmode local");
            wornBall = Cosmetics::Equipped(Cosmetics::Ball);
            wornHat = Cosmetics::Equipped(Cosmetics::Hat);
            wornBfx = Cosmetics::Equipped(Cosmetics::Bfx);
            if (Cosmetics::Equip(Cosmetics::Ball, "no-such-cosmetic"))
                return "Cosmetics::Equip of a missing cosmetic answered true";
            if (!Cosmetics::Equip(Cosmetics::Ball, "api-tests.model") || Cosmetics::Equipped(Cosmetics::Ball) != "api-tests.model")
                return "Cosmetics::Equipped '" + Cosmetics::Equipped(Cosmetics::Ball) + "' after Cosmetics::Equip";
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            // The menu ball wears it: the host builds the model on it.
            if (!LogSince("cosmetics: built api-tests.model on"))
                return WAIT;
            Cosmetics::Equip(Cosmetics::Hat, "api-tests.hat");
            Cosmetics::Equip(Cosmetics::Bfx, "api-tests.bfx");
            step = 2;
            return WAIT;
        }
        if (Elapsed() < 2)
            return WAIT;
        bool hat = Cosmetics::Equipped(Cosmetics::Hat) == "api-tests.hat";
        Cosmetics::Equip(Cosmetics::Ball, wornBall);            // back to what the player wore
        Cosmetics::Equip(Cosmetics::Hat, wornHat);
        Cosmetics::Equip(Cosmetics::Bfx, wornBfx);
        Console::Run("cosmode public");
        return Is(hat, "Cosmetics::Equipped(Hat) '" + Cosmetics::Equipped(Cosmetics::Hat) + "'");
    }, 20);
    Add("customize", "Cosmetics preview ball", "Cosmetics::PreviewBall", function() {
        double x, y, z, radius, facing;
        if (!Cosmetics::PreviewBall(x, y, z, radius, facing))
            return "Cosmetics::PreviewBall false with the Customize page shown";
        return Is(radius > 10 && radius < 100, "radius " + radius);
    });
    Add("customize", "Cosmetics extras", "Cosmetics::AddExtra,Cosmetics::EquipExtra,Cosmetics::EquippedExtra", function() {
        if (step == 0)
        {
            string f = Plugins::Folder();
            wornArms = Cosmetics::EquippedExtra("api-arms");
            array<string> c = {Is(Cosmetics::AddExtra("api-arms", "api-tests.arms", "api arms", f + "test.png", f + "shapes.txt"), "Cosmetics::AddExtra answered false"),
                               Is(!Cosmetics::AddExtra("Bad Slot!", "api-tests.bad-slot", "bad", f + "test.png", f + "shapes.txt"), "Cosmetics::AddExtra with a bad slot name answered true"),
                               Is(!Cosmetics::EquipExtra("api-arms", "no-such-extra"), "Cosmetics::EquipExtra of a missing extra answered true"),
                               Is(Cosmetics::EquipExtra("api-arms", "api-tests.arms") && Cosmetics::EquippedExtra("api-arms") == "api-tests.arms",
                                  "Cosmetics::EquippedExtra '" + Cosmetics::EquippedExtra("api-arms") + "' after Cosmetics::EquipExtra")};
            string failed = All(c);
            if (failed != "")
                return failed;
            step = 1;
            return WAIT;
        }
        // The menu ball wears it: the host builds its model on the ball.
        if (!LogSince("cosmetics: built api-tests.arms on"))
            return WAIT;
        bool off = Cosmetics::EquipExtra("api-arms", "") && Cosmetics::EquippedExtra("api-arms") == "";
        Cosmetics::EquipExtra("api-arms", wornArms);
        return Is(off, "Cosmetics::EquipExtra(\"\") left '" + Cosmetics::EquippedExtra("api-arms") + "'");
    }, 20);
}

// --- tracks (menu): Tracks ------------------------------------------------------------------------------------------------------
void RegisterTracks()
{
    Add("tracks", "Tracks official", "Tracks::Official,Tracks::Title,Tracks::Group,Tracks::Image", function() {
        array<string>@ levels = Tracks::Official();
        if (levels.length() == 0)
            return "Tracks::Official: no official tracks";
        level = levels[0];
        array<string> c = {Is(Tracks::Title(level) != "", "Tracks::Title('" + level + "') empty"), Is(Tracks::Group(level) != "", "Tracks::Group empty"),
                           Is(Tracks::Image(level) == "" || Tracks::Image(level).findFirst("/Game/") == 0, "Tracks::Image '" + Tracks::Image(level) + "'"),
                           Is(Tracks::Title("no-such-level") == "", "Tracks::Title of a missing level not empty")};
        return All(c);
    });
    Add("tracks", "Tracks search", "Tracks::Search,Tracks::SearchState,Tracks::ResultCount,Tracks::Total,Tracks::ResultTitle,Tracks::ResultId,Tracks::ResultImage", function() {
        if (step == 0)
        {
            Tracks::Search("sky");
            step = 1;
            return WAIT;
        }
        string state = Tracks::SearchState();
        if (state.findFirst("error") == 0)
            return "Tracks::SearchState " + state;
        if (state != "done")
            return WAIT;
        if (Tracks::ResultCount() == 0)
            return "Tracks::ResultCount: no results for 'sky'";
        array<string> c = {Is(Tracks::Total() >= Tracks::ResultCount(), "Tracks::Total " + Tracks::Total() + " < ResultCount " + Tracks::ResultCount()),
                           Is(Tracks::ResultTitle(0) != "", "Tracks::ResultTitle(0) empty"), Is(parseInt(Tracks::ResultId(0)) > 0, "Tracks::ResultId(0) '" + Tracks::ResultId(0) + "'"),
                           Is(Tracks::ResultImage(99999) == "", "Tracks::ResultImage past the end not empty"), Is(Tracks::ResultTitle(99999) == "", "Tracks::ResultTitle past the end not empty")};
        return All(c);
    }, 30);
    Add("tracks", "Workshop find newest", "Workshop::Find,Workshop::State,Workshop::Count,Workshop::Total,Workshop::Id,Workshop::Title,Workshop::Author,Workshop::Description,Workshop::Tags,Workshop::Created,Workshop::Updated,Workshop::VotesUp,Workshop::VotesDown,Workshop::Score,Workshop::Plays,Workshop::Subscribers,Workshop::Favorites,Workshop::Size,Workshop::Image,Workshop::Forget", function() {
        if (step == 0)
        {
            id1 = Workshop::Find("", "new");
            if (id1 < 0)
                return "Workshop::Find answered -1";
            step = 1;
            return WAIT;
        }
        string state = Workshop::State(id1);
        if (state.findFirst("error") == 0)
            return "Workshop::State " + state;
        if (state != "done")
            return WAIT;
        if (Workshop::Count(id1) == 0)
            return "Workshop::Count: no maps";
        string id = Workshop::Id(id1, 0);
        if (step == 1)
        {
            s1 = id;
            step = 2;
        }
        // The preview downloads after the first ask.
        if (Workshop::Image(s1) == "" && Elapsed() < 15)
            return WAIT;
        array<string> c = {Is(Workshop::Count(id1) == 50, "Workshop::Count " + Workshop::Count(id1) + " on a first page of 50"),
                           Is(Workshop::Total(id1) >= Workshop::Count(id1), "Workshop::Total " + Workshop::Total(id1)),
                           Is(parseInt(id) > 0, "Workshop::Id(0) '" + id + "'"), Is(Workshop::Id(id1, 999) == "", "Workshop::Id past the end not empty"),
                           Is(Workshop::Title(id) != "", "Workshop::Title empty"), Is(parseInt(Workshop::Author(id)) > 0, "Workshop::Author '" + Workshop::Author(id) + "'"),
                           Is(Workshop::Description(id).length() < 8000, "Workshop::Description too long"),
                           Is(Workshop::Tags(id).length() < 1025, "Workshop::Tags too long"),
                           Is(Workshop::Created(id) > 1700000000, "Workshop::Created " + Workshop::Created(id)),
                           Is(Workshop::Updated(id) >= Workshop::Created(id), "Workshop::Updated " + Workshop::Updated(id) + " before Created"),
                           Is(Workshop::Created(Workshop::Id(id1, 0)) >= Workshop::Created(Workshop::Id(id1, Workshop::Count(id1) - 1)), "Workshop::Find new: not newest first"),
                           Is(Workshop::VotesUp(id) >= 0 && Workshop::VotesDown(id) >= 0, "Workshop::VotesUp/VotesDown negative"),
                           Is(Workshop::Score(id) >= 0 && Workshop::Score(id) <= 1, "Workshop::Score " + Workshop::Score(id)),
                           Is(Workshop::Plays(id) >= 0 && Workshop::Subscribers(id) >= 0 && Workshop::Favorites(id) >= 0, "Workshop::Plays/Subscribers/Favorites negative"),
                           Is(Workshop::Size(id) > 0, "Workshop::Size " + Workshop::Size(id)),
                           Is(Workshop::Image(s1) != "", "Workshop::Image still empty after 15 s"),
                           Is(Workshop::Title("1") == "", "Workshop::Title of an unknown map not empty")};
        Workshop::Forget(id1);
        c.insertLast(Is(Workshop::State(id1) == "", "Workshop::Forget: State still '" + Workshop::State(id1) + "'"));
        c.insertLast(Is(Workshop::Title(id) != "", "Workshop::Forget also forgot the map"));
        return All(c);
    }, 40);
    Add("tracks", "Workshop sorts and tags", "Workshop::Find,Workshop::State,Workshop::Count,Workshop::Tags", function() {
        if (step == 0)
        {
            if (Workshop::Find("", "no-such-sort") != -1)
                return "Workshop::Find with an unknown sort didn't answer -1";
            array<string> with = {"beginner"}, none;
            id1 = Workshop::Find("", "top", 1, 7, with, none);
            array<string> without = {"beginner"};
            id2 = Workshop::Find("", "trending", 1, 30, none, without);
            id3 = Workshop::Find("sky", "relevance");
            step = 1;
            return WAIT;
        }
        for (int q = 0; q < 3; q++)
        {
            int query = q == 0 ? id1 : q == 1 ? id2 : id3;
            string state = Workshop::State(query);
            if (state.findFirst("error") == 0)
                return "Workshop::State " + state;
            if (state != "done")
                return WAIT;
        }
        string bad = "";
        for (int i = 0; i < Workshop::Count(id1); i++)
            if (Workshop::Tags(Workshop::Id(id1, i)).findFirst("beginner") < 0)
                bad = "with beginner: '" + Workshop::Tags(Workshop::Id(id1, i)) + "'";
        for (int i = 0; i < Workshop::Count(id2); i++)
            if (Workshop::Tags(Workshop::Id(id2, i)).findFirst("beginner") >= 0)
                bad = "without beginner: '" + Workshop::Tags(Workshop::Id(id2, i)) + "'";
        array<string> c = {Is(Workshop::Count(id1) > 0, "Workshop::Find top with beginner: no maps"), Is(Workshop::Count(id2) > 0, "Workshop::Find trending: no maps"),
                           Is(Workshop::Count(id3) > 0, "Workshop::Find relevance 'sky': no maps"), Is(bad == "", "Workshop::Find tags " + bad)};
        Workshop::Forget(id1);
        Workshop::Forget(id2);
        Workshop::Forget(id3);
        return All(c);
    }, 40);
    Add("tracks", "Workshop lists, ids and names", "Workshop::FindList,Workshop::FindIds,Workshop::Me,Workshop::Name,Workshop::Author", function() {
        if (step == 0)
        {
            id1 = Workshop::Find("", "top");
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Workshop::State(id1) != "done")
                return Workshop::State(id1).findFirst("error") == 0 ? "Workshop::State " + Workshop::State(id1) : WAIT;
            s1 = Workshop::Id(id1, 0);
            if (Workshop::FindList("no-such-list") != -1)
                return "Workshop::FindList with an unknown list didn't answer -1";
            id2 = Workshop::FindList("published", Workshop::Author(s1));
            array<string> ids = {s1};
            id3 = Workshop::FindIds(ids);
            step = 2;
            return WAIT;
        }
        if (Workshop::State(id2) != "done" || Workshop::State(id3) != "done")
        {
            if (Workshop::State(id2).findFirst("error") == 0 || Workshop::State(id3).findFirst("error") == 0)
                return "Workshop::State " + Workshop::State(id2) + " / " + Workshop::State(id3);
            return WAIT;
        }
        // Steam has the author's name a moment after the first ask.
        if (Workshop::Name(Workshop::Author(s1)) == "" && Elapsed() < 20)
            return WAIT;
        string author = Workshop::Author(s1), bad = "";
        bool found = false;
        for (int i = 0; i < Workshop::Count(id2); i++)
        {
            if (Workshop::Author(Workshop::Id(id2, i)) != author)
                bad = Workshop::Id(id2, i) + " by " + Workshop::Author(Workshop::Id(id2, i));
            if (Workshop::Id(id2, i) == s1)
                found = true;
        }
        array<string> c = {Is(parseInt(Workshop::Me()) > 0, "Workshop::Me '" + Workshop::Me() + "'"),
                           Is(bad == "", "Workshop::FindList published: a map " + bad), Is(found || Workshop::Total(id2) > 50, "Workshop::FindList published: the author's map isn't there"),
                           Is(Workshop::Count(id3) == 1 && Workshop::Id(id3, 0) == s1, "Workshop::FindIds: " + Workshop::Count(id3) + " maps"),
                           Is(Workshop::Name(author) != "", "Workshop::Name of the author still empty after 20 s"),
                           Is(Workshop::Name(Workshop::Me()) != "", "Workshop::Name of yourself empty")};
        Workshop::Forget(id1);
        Workshop::Forget(id2);
        Workshop::Forget(id3);
        return All(c);
    }, 40);
    Add("tracks", "Workshop progress", "Workshop::Finished,Workshop::MyMedal,Workshop::MyBest,Workshop::MyRank,Workshop::Players", function() {
        array<string>@ finished = Workshop::Finished();
        if (finished.length() == 0)
            return "Workshop::Finished: no finished workshop maps in this save (finish one to test this)";
        if (step == 0)
        {
            id1 = Workshop::FindIds(finished);
            s1 = finished[0];
            step = 1;
            return WAIT;
        }
        if (Workshop::State(id1) != "done")
            return Workshop::State(id1).findFirst("error") == 0 ? "Workshop::State " + Workshop::State(id1) : WAIT;
        if (Workshop::MyRank(s1) == -1)
            return WAIT;
        int medal = Workshop::MyMedal(s1);
        array<string> c = {Is(medal >= 0 && medal <= 4, "Workshop::MyMedal " + medal), Is(Workshop::MyMedal("1") == -1, "Workshop::MyMedal of an unknown map " + Workshop::MyMedal("1")),
                           Is(Workshop::MyBest(s1) > 0, "Workshop::MyBest " + Workshop::MyBest(s1)), Is(Workshop::MyBest("1") == 0, "Workshop::MyBest of an unknown map"),
                           Is(Workshop::MyRank(s1) > 0, "Workshop::MyRank " + Workshop::MyRank(s1) + " on a finished map"),
                           Is(Workshop::Players(s1) >= Workshop::MyRank(s1), "Workshop::Players " + Workshop::Players(s1) + " below MyRank " + Workshop::MyRank(s1))};
        Log::Info("workshop progress: " + finished.length() + " finished; " + s1 + " medal " + medal + " best " + Workshop::MyBest(s1) + " rank " +
                  Workshop::MyRank(s1) + " of " + Workshop::Players(s1));
        Workshop::Forget(id1);
        return All(c);
    }, 40);
    Add("tracks", "Hub off screen", "Hub::Shown,Hub::Entries,Hub::Focused,Hub::FocusedAuthor,Hub::HideEntry,Hub::ListShown,Hub::View,Hub::Thumbnails,Hub::SetEntryBadge,Hub::SetAuthorButton,Hub::AuthorButtonClicked", function() {
        // The run starts on the main menu with the play page showing the game's own tracks, not the hub.
        array<string> c = {Is(!Hub::Shown(), "Hub::Shown true on the main menu"),
                           Is(!Hub::HideEntry("1", true), "Hub::HideEntry of a map not on the list answered true"),
                           Is(Hub::Focused() == "" || parseInt(Hub::Focused()) > 0, "Hub::Focused '" + Hub::Focused() + "'"),
                           Is(Hub::FocusedAuthor() == "" || parseInt(Hub::FocusedAuthor()) > 0, "Hub::FocusedAuthor '" + Hub::FocusedAuthor() + "'"),
                           Is(Hub::Entries().length() <= 50, "Hub::Entries " + Hub::Entries().length()),
                           Is(!Hub::ListShown(), "Hub::ListShown true on the main menu"),
                           Is(Hub::View() == "", "Hub::View '" + Hub::View() + "' on the main menu"),
                           Is(Hub::Thumbnails().length() == 0, "Hub::Thumbnails " + Hub::Thumbnails().length() + " on the main menu"),
                           Is(!Hub::SetEntryBadge("1", "7"), "Hub::SetEntryBadge of a map not on screen answered true")};
        Hub::SetAuthorButton("");
        c.insertLast(Is(!Hub::AuthorButtonClicked(), "Hub::AuthorButtonClicked true without a button"));
        return All(c);
    });
    Add("tracks", "Hub search", "Hub::Search,Hub::Shown,Hub::Entries,Hub::HideEntry,Window.DockInHub,Hub::ListShown,Hub::View,Hub::Thumbnails,Hub::SetEntryBadge,Hub::SetAuthorButton", function() {
        // Opens the hub (the play page's workshop side) the way its own tab does, then searches in it.
        if (step == 0)
        {
            Console::Run("hubopen");
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (!Hub::Shown())
                return Elapsed() > 15 ? "Hub::Shown still false after opening the hub" : WAIT;
            @win = UI::CreateWindow();
            win.DockInHub();
            win.AddText("api tests", 16);
            if (!Hub::Search("sky", "relevance"))
                return "Hub::Search answered false";
            step = 2;
            t0 = Host::Time();
            return WAIT;
        }
        array<string>@ ids = Hub::Entries();
        if (ids.length() == 0)
            return Elapsed() > 15 ? "Hub::Entries empty 15 s after the search" : WAIT;
        if (step == 2)
        {
            if (!Hub::HideEntry(ids[0], true))
                return "Hub::HideEntry answered false for a map on the list";
            s1 = ids[0];
            step = 3;
            return WAIT;
        }
        bool stillListed = Hub::Entries().find(s1) >= 0;
        Hub::HideEntry(s1, false);
        array<string>@ shown = Hub::Thumbnails();
        bool badged = shown.length() > 0 && Hub::SetEntryBadge(shown[0], "7");
        if (shown.length() > 0)
            Hub::SetEntryBadge(shown[0], "");
        Hub::SetAuthorButton("api tests");
        Hub::SetAuthorButton("");
        win.visible = false;
        array<string> c = {Is(stillListed, "Hub::Entries left out a map Hub::HideEntry hid"),
                           Is(Hub::ListShown(), "Hub::ListShown false with the search's list on screen"),
                           Is(Hub::View() == "list", "Hub::View '" + Hub::View() + "' with the list on screen"),
                           Is(shown.find(ids[0]) >= 0, "Hub::Thumbnails left out the list's first map"),
                           Is(badged, "Hub::SetEntryBadge answered false for a map on screen")};
        return All(c);
    }, 40);
    Add("tracks", "Ghosts load a leaderboard", "Ghosts::Load,Ghosts::State,Ghosts::Leaderboard,Ghosts::Entries,Ghosts::WithoutReplay,Ghosts::Count", function() {
        if (level == "")
            return "no level from Tracks::Official";
        if (step == 0)
        {
            if (!Ghosts::Load(level, 5))
                return "Ghosts::Load answered false";
            step = 1;
            return WAIT;
        }
        string state = Ghosts::State();
        if (state.findFirst("error") == 0)
            return "State " + state;
        if (state != "ready")
            return WAIT;
        array<string> c = {Is(Ghosts::Count() >= 5, "Ghosts::Count " + Ghosts::Count() + " for the top 5 (+ own)"), Is(Ghosts::Leaderboard() != "", "Ghosts::Leaderboard empty"),
                           Is(Ghosts::Entries() > 5, "Ghosts::Entries " + Ghosts::Entries()), Is(Ghosts::WithoutReplay() >= 0, "Ghosts::WithoutReplay " + Ghosts::WithoutReplay())};
        return All(c);
    }, 60);
    Add("track", "Ghosts load the track on screen", "Ghosts::Load,Ghosts::State,Ghosts::Count", function() {
        if (step == 0)
        {
            if (!Ghosts::Load("", 5))
                return Elapsed() > 20 ? "Ghosts::Load(\"\") answered false for 20 s" : WAIT;
            step = 1;
            return WAIT;
        }
        string state = Ghosts::State();
        if (state.findFirst("error") == 0)
            return "State " + state;
        if (state != "ready")
            return WAIT;
        return Is(Ghosts::Count() >= 5, "Ghosts::Count " + Ghosts::Count());
    }, 60);
    Add("track", "Ghosts runs", "Ghosts::Name,Ghosts::Rank,Ghosts::Time,Ghosts::IsOwn,Ghosts::SampleCount,Ghosts::Sample,Ghosts::Position,Ghosts::Splits", function() {
        if (Ghosts::Count() == 0)
            return "no ghosts loaded";
        double t, x, y, z, px, py, pz;
        bool sampled = Ghosts::Sample(0, 1, t, x, y, z);
        bool placed = Ghosts::Position(0, t, px, py, pz);
        int own = 0;
        for (int i = 0; i < Ghosts::Count(); i++)
            if (Ghosts::IsOwn(i))
                own++;
        array<string> c = {Is(Ghosts::Name(0) != "", "Ghosts::Name(0) empty"), Is(Ghosts::Rank(0) >= 1, "Ghosts::Rank(0) " + Ghosts::Rank(0)),
                           Is(Ghosts::Time(0) > 1, "Ghosts::Time(0) " + Ghosts::Time(0)), Is(own <= 1, "Ghosts::IsOwn: " + own + " runs are own"),
                           Is(Ghosts::SampleCount(0) > 10, "Ghosts::SampleCount(0) " + Ghosts::SampleCount(0)),
                           Is(sampled, "Ghosts::Sample(0, 1) false"), Is(placed && Abs(px - x) < 1 && Abs(py - y) < 1 && Abs(pz - z) < 1, "Ghosts::Position at a sample's time is not the sample"),
                           Is(Ghosts::Name(99999) == "", "Ghosts::Name past the end not empty")};
        // Splits come from the replays: one per checkpoint taken, none on a track without checkpoints.
        int withSplits = 0;
        for (int i = 0; i < Ghosts::Count(); i++)
            if (Ghosts::Splits(i).length() > 0)
                withSplits++;
        // (the finish is one of the checkpoints: a track with only a finish records no splits, measured on two)
        if (Ghosts::CheckpointCount() > 1)
            c.insertLast(Is(withSplits > 0, "Ghosts::Splits: no run has splits on a track with " + Ghosts::CheckpointCount() + " checkpoints"));
        else
            Log::Info("splits not checked: " + level + " has no checkpoints besides the finish");
        return All(c);
    });
}

// --- replay (a simulated one): Replay -----------------------------------------------------------------------------------------
void RegisterReplay()
{
    Add("replay", "Replay time", "Replay::IsActive,Replay::Time,Replay::Length,Replay::Seek,Replay::Restart", function() {
        if (step == 0)
        {
            d1 = Replay::Time();
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 1)
                return WAIT;
            if (Replay::Time() <= d1)
                return "Replay::Time did not advance: " + d1 + " then " + Replay::Time();
            if (Abs(Replay::Length() - 30) > 0.01)
                return "Replay::Length " + Replay::Length() + ", want 30";
            Replay::Seek(15);
            step = 2;
            return WAIT;
        }
        if (step == 2)
        {
            if (Replay::Time() < 14.9 || Replay::Time() > 16)
                return Elapsed() > 3 ? "Replay::Time " + Replay::Time() + " after Replay::Seek(15)" : WAIT;
            Replay::Restart();
            step = 3;
            return WAIT;
        }
        if (Replay::Time() > 1.5)
            return Elapsed() > 5 ? "Replay::Time " + Replay::Time() + " after Replay::Restart" : WAIT;
        return Is(Replay::IsActive(), "Replay::IsActive false during the replay");
    }, 12);
    Add("replay", "Replay camera", "Replay::CameraMode,Replay::SetCameraMode,Replay::CameraDistance,Replay::SetCameraDistance,Replay::SeeThrough,Replay::SetSeeThrough", function() {
        int mode = Replay::CameraMode();
        double distance = Replay::CameraDistance();
        bool through = Replay::SeeThrough();
        Replay::SetCameraMode(Replay::Follow3D);
        Replay::SetCameraDistance(700);
        Replay::SetSeeThrough(!through);
        array<string> c = {Is(Replay::CameraMode() == int(Replay::Follow3D), "Replay::CameraMode " + Replay::CameraMode() + " after Replay::SetCameraMode(Follow3D)"),
                           Near(Replay::CameraDistance(), 700, "Replay::CameraDistance after Replay::SetCameraDistance", 0.5), Is(Replay::SeeThrough() == !through, "Replay::SeeThrough not changed by Replay::SetSeeThrough")};
        Replay::SetCameraMode(mode);
        Replay::SetCameraDistance(distance);
        Replay::SetSeeThrough(through);
        return All(c);
    });
}

// --- track (before the race): Race info, Leaderboard, Hud, Ghosts in the world, Draw, Camera --------------------------
string hudKey = "";

void RegisterTrack()
{
    Add("track", "Tracks::Open reached the track", "Tracks::Open,Tracks::OpenState", function() {
        string state = Tracks::OpenState();
        return Is(Race::OnTrack() && state.findFirst("error") != 0, "Tracks::Open: on track " + Race::OnTrack() + ", Tracks::OpenState '" + state + "'");
    });
    Add("track", "Race track info", "Race::OnTrack,Race::TrackKey,Race::TrackName,Race::TrackAuthor,Race::AuthorTime,Race::IsCustomTrack,Race::IsActive,Race::IsComplete,Host::MapNumber", function() {
        if (Race::AuthorTime() <= 0)
            return WAIT;
        array<string> c = {Is(Race::OnTrack(), "Race::OnTrack false"), Is(Race::TrackKey() == "map:" + level, "Race::TrackKey '" + Race::TrackKey() + "', want map:" + level),
                           Is(Race::TrackName() != "", "Race::TrackName empty"), Is(Race::TrackAuthor().length() < 200, "Race::TrackAuthor '" + Race::TrackAuthor() + "'"),
                           Is(!Race::IsCustomTrack(), "Race::IsCustomTrack true on an official track"), Is(!Race::IsActive(), "Race::IsActive before the race"),
                           Is(!Race::IsComplete(), "Race::IsComplete before the race"), Is(Host::MapNumber() > 0, "Host::MapNumber " + Host::MapNumber())};
        return All(c);
    }, 20);
    Add("ui", "Leaderboard overall players and note", "Leaderboard::OverallPlayers,Leaderboard::SetOverallNote", function() {
        if (Leaderboard::OverallPlayers() <= 0)
            return WAIT;                    // the bar finds its board a moment after the menu shows
        Leaderboard::SetOverallNote("api note");
        Leaderboard::SetOverallNote("");
        return PASS;
    }, 30);
    Add("track", "Leaderboard players and note", "Leaderboard::Players,Leaderboard::SetTitleNote", function() {
        if (Leaderboard::Players() <= 0)
            return WAIT;
        Leaderboard::SetTitleNote("· api note");
        Leaderboard::SetTitleNote("");
        return PASS;
    }, 30);
    Add("track", "Hud parts", "Hud::Elements,Hud::Name,Hud::Label,Hud::Shown,Hud::ParentShown", function() {
        array<string>@ parts = Hud::Elements();
        if (parts.length() == 0)
            return WAIT;
        hudKey = "";
        for (uint i = 0; i < parts.length(); i++)
            if (parts[i] == "RaceUI/WBP_Leaderboard_0" && Hud::Shown(parts[i]))
                hudKey = parts[i];
        for (uint i = 0; i < parts.length() && hudKey == ""; i++)
            if (Hud::Shown(parts[i]) && Hud::ParentShown(parts[i]) && parts[i].findFirst("RaceUI/") == 0 && parts[i].findFirst("Countdown") < 0)
                hudKey = parts[i];
        array<string> c = {Is(hudKey != "", "Hud::Elements/Hud::Shown: no shown RaceUI part among " + parts.length()), Is(Hud::Name(parts[0]) != "", "Hud::Name empty"),
                           Is(Hud::Label("no/such") == "", "Hud::Label of a missing part not empty"), Is(Hud::Name("no/such") == "", "Hud::Name of a missing part not empty")};
        return All(c);
    }, 20);
    Add("track", "Hud layout", "Hud::SetLayout,Hud::ClearLayout,Hud::SetEditing,Hud::SetBlink", function() {
        // "Off" draws the part at opacity 0 (the host's "hud" command reports it); Shown stays what the game shows.
        if (hudKey == "")
            return "no HUD part";
        if (step == 0)
        {
            Hud::SetLayout(hudKey, 0.5, 0.5, 1, Hud::Off);
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 1)
                return WAIT;
            mark = Log::LineCount();
            Console::Run("hud");
            step = 2;
            return WAIT;
        }
        if (step == 2)
        {
            string line = LogLineSince("hud: " + hudKey + " ");
            if (line == "")
                return WAIT;
            if (line.findFirst("opacity 0.00") < 0)
                return "Hud::SetLayout(Off): " + line;
            Hud::SetEditing(true);          // an "off" part shows faintly while editing
            step = 3;
            t0 = Host::Time();
            return WAIT;
        }
        if (step == 3)
        {
            if (Elapsed() < 1)
                return WAIT;
            mark = Log::LineCount();
            Console::Run("hud");
            step = 4;
            return WAIT;
        }
        if (step == 4)
        {
            string line = LogLineSince("hud: " + hudKey + " ");
            if (line == "")
                return WAIT;
            if (line.findFirst("opacity 0.25") < 0)
                return "Hud::SetEditing: while editing, an off part: " + line;
            Hud::SetEditing(false);
            Hud::SetBlink(hudKey);
            Hud::SetBlink("");
            Hud::ClearLayout(hudKey);
            step = 5;
            t0 = Host::Time();
            return WAIT;
        }
        if (step == 5)
        {
            if (Elapsed() < 1)
                return WAIT;
            mark = Log::LineCount();
            Console::Run("hud");
            step = 6;
            return WAIT;
        }
        string line = LogLineSince("hud: " + hudKey + " ");
        if (line == "")
            return WAIT;
        return Is(line.findFirst("opacity 1.00") >= 0, "Hud::ClearLayout: after it, " + line);
    }, 15);
    Add("track", "Hud part colour", "Hud::SetPartColor,Hud::ResetPartColor", function() {
        array<string>@ parts = Hud::Elements();
        bool found = false;
        for (uint i = 0; i < parts.length(); i++)
            found = found || parts[i] == "PlayerUI/WBP_InputVisualizer";
        if (!found)
            return "Hud::Elements: no PlayerUI/WBP_InputVisualizer part";
        if (Hud::SetPartColor("PlayerUI/WBP_InputVisualizer", "no_such_part", 1, 0, 0, 1))
            return "Hud::SetPartColor of a missing part answered true";
        if (!Hud::SetPartColor("PlayerUI/WBP_InputVisualizer", "Key_Jump", 1, 0.1f, 0.1f, 0.85f))
            return "Hud::SetPartColor(Key_Jump) answered false";
        Hud::ResetPartColor("PlayerUI/WBP_InputVisualizer", "Key_Jump");
        return PASS;
    });
    Add("track", "Hud hide the game", "Hud::HideGame", function() {
        if (step == 0)
        {
            Hud::HideGame(true);
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            Hud::Elements();
            if (hudKey != "" && Hud::Shown(hudKey) && Hud::ParentShown(hudKey))
                return Elapsed() > 3 ? "Hud::HideGame: " + hudKey + " still shown with the game hidden" : WAIT;
            Hud::HideGame(false);
            step = 2;
            return WAIT;
        }
        return Elapsed() > 1.5 ? PASS : WAIT;
    }, 10);
    Add("track", "Ghosts on the track", "Ghosts::CheckpointCount,Ghosts::Checkpoint,Ghosts::CheckpointOrder,Ghosts::View", function() {
        if (Ghosts::Count() == 0)
            return "no ghosts loaded";
        if (Ghosts::CheckpointCount() == 0)
            return Elapsed() > 5 ? "SKIP: this track has no checkpoints (" + level + ")" : WAIT;
        int number;
        double x, y, z, pitch, yaw, fov;
        bool cp = Ghosts::Checkpoint(0, number, x, y, z);
        bool view = Ghosts::View(0, 1.0, x, y, z, pitch, yaw, fov);
        array<string> c = {Is(cp && number >= 1, "Ghosts::Checkpoint(0) " + cp + " number " + number), Is(view && fov > 10 && fov < 170, "Ghosts::View(0, 1 s) " + view + " fov " + fov)};
        if (Ghosts::CheckpointCount() > 1)
            c.insertLast(Is(Ghosts::CheckpointOrder(0).length() > 0, "Ghosts::CheckpointOrder(0) empty with " + Ghosts::CheckpointCount() + " checkpoints"));
        return All(c);
    }, 20);
    Add("track", "Ghosts balls", "Ghosts::PlayerBall,Ghosts::PlaceBall,Ghosts::ShowBallName", function() {
        int id = Ghosts::PlayerBall(0);
        if (id <= 0)
            return "Ghosts::PlayerBall(0) " + id;
        array<string> c = {Is(Ghosts::PlaceBall(id, 0, 2.0), "Ghosts::PlaceBall false"), Is(Ghosts::ShowBallName(id, true), "Ghosts::ShowBallName false"),
                           Is(!Ghosts::PlaceBall(999999, 0, 2.0), "Ghosts::PlaceBall of a missing ball true")};
        Draw::Remove(id);
        return All(c);
    });
    Add("track", "Ghosts crowd", "Ghosts::CrowdCreate,Ghosts::CrowdMembers,Ghosts::CrowdSkins,Ghosts::CrowdTrails,Ghosts::CrowdTrailsUpTo,Ghosts::CrowdShowTrails,Ghosts::CrowdTimes,Ghosts::CrowdPlace", function() {
        array<float> palette = {1, 0, 0, 0, 0, 1};
        int id = Ghosts::CrowdCreate(40, palette);
        if (id <= 0)
            return "Ghosts::CrowdCreate " + id;
        array<int> members;
        array<int> groups;
        array<double> offsets;
        array<bool> shown;
        for (int i = 0; i < Ghosts::Count(); i++)
        {
            members.insertLast(i);
            groups.insertLast(i % 2);
            offsets.insertLast(0);
            shown.insertLast(true);
        }
        array<string> c = {Is(Ghosts::CrowdMembers(id, members, groups), "Ghosts::CrowdMembers false"), Is(Ghosts::CrowdSkins(id), "Ghosts::CrowdSkins false"),
                           Is(Ghosts::CrowdTrails(id, 4, 0.25f), "Ghosts::CrowdTrails false"), Is(Ghosts::CrowdTrailsUpTo(id, 5), "Ghosts::CrowdTrailsUpTo false"),
                           Is(Ghosts::CrowdShowTrails(id, false), "Ghosts::CrowdShowTrails false"), Is(Ghosts::CrowdTimes(id, offsets, shown), "Ghosts::CrowdTimes false"),
                           Is(Ghosts::CrowdPlace(id, 3), "Ghosts::CrowdPlace false"), Is(!Ghosts::CrowdPlace(999999, 3), "Ghosts::CrowdPlace of a missing crowd true")};
        Draw::Remove(id);
        return All(c);
    });
    Add("race", "Draw shapes", "Draw::Tube,Draw::Glow,Draw::Fade,Draw::Ball,Draw::Move,Draw::Show,Draw::Remove,Draw::Clear", function() {
        double x, y, z;
        if (!Race::BallPosition(x, y, z))
            return "no ball position to draw near";
        array<double> path = {x, y, z + 100, x + 200, y, z + 150, x + 400, y + 100, z + 150};
        int glow = Draw::Tube(path, 5, 0.2f, 0.8f, 0.4f, true);
        int glass = Draw::Tube(path, 5, 0.2f, 0.8f, 0.4f, false, 0.3f);
        int ball = Draw::Ball(20, 1, 0.5f, 0.2f, true);
        if (glow <= 0 || glass <= 0 || ball <= 0)
            return "Draw::Tube/Draw::Ball ids " + glow + ", " + glass + ", " + ball;
        array<string> c = {Is(Draw::Glow(glow, 1, 0, 0, 10), "Draw::Glow false"), Is(Draw::Fade(glass, 0.5f), "Draw::Fade false"),
                           Is(Draw::Move(ball, x, y, z + 200), "Draw::Move false"), Is(Draw::Show(ball, false), "Draw::Show false")};
        Draw::Remove(ball);
        c.insertLast(Is(!Draw::Move(ball, x, y, z), "Draw::Remove: Draw::Move after it answered true"));
        Draw::Clear();
        c.insertLast(Is(!Draw::Show(glow, true), "Draw::Clear: Draw::Show after it answered true"));
        return All(c);
    });
    Add("race", "Draw models and effects", "Draw::Model,Draw::Turn,Draw::Scale,Draw::Effect,Draw::Sound,Camera::Shake", function() {
        double x, y, z;
        if (!Race::BallPosition(x, y, z))
            return "no ball position to draw near";
        int model = Draw::Model(Plugins::Folder() + "shapes.txt");
        if (model <= 0)
            return "Draw::Model(shapes.txt) " + model;
        const string wave = "/Game/Packs/Vefects/Easy_Shockwaves_VFX/VFX/Shockwaves/Particles/VFX_Shockwave_01_White_1s.VFX_Shockwave_01_White_1s";
        array<string> c = {Is(Draw::Model(Plugins::Folder() + "missing.txt") == 0, "Draw::Model of a missing file not 0"),
                           Is(Draw::Move(model, x, y, z + 150), "Draw::Move of a model false"), Is(Draw::Turn(model, 10, 45, 0), "Draw::Turn false"),
                           Is(Draw::Scale(model, 0.5), "Draw::Scale false"), Is(Draw::Effect(wave, x, y, z - 47, 0.5), "Draw::Effect false"),
                           Is(!Draw::Effect("/Game/NoSuchEffect.NoSuchEffect", x, y, z), "Draw::Effect of a missing system true"),
                           Is(Draw::Sound("/Game/Sound/Gameplay/SFX_SoftPop.SFX_SoftPop", 0.3), "Draw::Sound false"),
                           Is(Draw::Sound(Plugins::Folder() + "beep.wav", 0.3), "Draw::Sound of the plugin's beep.wav false"),
                           Is(!Draw::Sound(Plugins::Folder() + "missing.wav"), "Draw::Sound of a missing .wav true"),
                           Is(Camera::Shake(0.2), "Camera::Shake false")};
        Draw::Remove(model);
        c.insertLast(Is(!Draw::Turn(model, 0, 0, 0), "Draw::Remove: Draw::Turn after it answered true"));
        return All(c);
    });
    Add("race", "Race::NextBounce", "Race::NextBounce", function() {
        double s, x, y, z, nx, ny, nz;
        bool ground;
        if (step == 0)
        {
            while (Race::NextBounce(s, x, y, z, nx, ny, nz, ground)) {}     // the settling bounces before this test
            Console::Run("fling 0 0 1200");       // up, to land again
            step = 1;
            return WAIT;
        }
        while (Race::NextBounce(s, x, y, z, nx, ny, nz, ground))
        {
            double bx, by, bz;
            Race::BallPosition(bx, by, bz);
            if (!ground || nz < 0.5)
                continue;
            array<string> c = {Is(s > 0 && s <= 1, "strength " + s), Is(Math::abs(bz - z - 47.5) < 60, "contact " + z + " below the ball at " + bz)};
            return All(c);
        }
        return Elapsed() > 6 ? "no ground bounce within 6 s of a fling up" : WAIT;
    }, 15);
    Add("race", "PostProcess", "PostProcess::Set,PostProcess::SetWeight,PostProcess::Clear", function() {
        array<string> c = {Is(PostProcess::Set("ColorSaturation", 0, 0, 0, 1), "PostProcess::Set ColorSaturation false"),
                           Is(PostProcess::Set("VignetteIntensity", 0.8), "PostProcess::Set VignetteIntensity false"),
                           Is(!PostProcess::Set("NoSuchSetting", 1), "PostProcess::Set of a missing setting true"),
                           Is(PostProcess::SetWeight(0.5), "PostProcess::SetWeight false")};
        PostProcess::Clear();
        return All(c);
    });
    Add("race", "Camera", "Camera::Project,Camera::Take,Camera::Set,Camera::Release,Camera::IsTaken", function() {
        double x, y, z;
        if (!Race::BallPosition(x, y, z))
            return "no ball position";
        if (step == 0)
        {
            if (!Camera::Take() || !Camera::IsTaken())
                return "Camera::Take/Camera::IsTaken false";
            if (!Camera::Set(x - 800, y, z + 300, -15, 0, 90))
                return "Camera::Set false";
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 1)
                return WAIT;
            float sx, sy;
            if (!Camera::Project(x, y, z, sx, sy))
                return "Camera::Project of the ball in front of the camera false";
            Camera::Release();
            if (Camera::IsTaken())
                return "Camera::IsTaken true after Camera::Release";
            step = 2;
            return WAIT;
        }
        return Elapsed() > 2 ? PASS : WAIT;
    });
    Add("track", "Race::HideBall", "Race::HideBall", function() {
        if (step == 0)
        {
            Race::HideBall(true);
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 1)
                return WAIT;
            Console::Run("ballstate");
            step = 2;
            return WAIT;
        }
        if (step == 2)
        {
            string line = LogLineSince("test: ball ");
            if (line == "")
                return WAIT;
            if (line.findFirst("actor hidden") < 0)
                return "Race::HideBall(true): ballstate " + line;
            Race::HideBall(false);
            mark = Log::LineCount();
            t0 = Host::Time();
            step = 3;
            return WAIT;
        }
        if (step == 3)
        {
            if (Elapsed() < 1)
                return WAIT;
            Console::Run("ballstate");
            step = 4;
            return WAIT;
        }
        string after = LogLineSince("test: ball ");
        if (after == "")
            return WAIT;
        return Is(after.findFirst("actor shown") >= 0, "Race::HideBall(false): ballstate " + after);
    });
}

// --- race: the race running -------------------------------------------------------------------------------------------------
void RegisterRace()
{
    Add("race", "Race running", "Race::IsActive,Race::RunId,Race::GetInput,Race::BallPosition", function() {
        double x, y, jumpX;
        bool jump;
        double bx, by, bz;
        array<string> c = {Is(Race::IsActive(), "Race::IsActive false"), Is(Race::RunId() >= 0, "Race::RunId " + Race::RunId()),
                           Is(Race::GetInput(x, y, jump), "Race::GetInput false"), Is(Race::BallPosition(bx, by, bz), "Race::BallPosition false")};
        return All(c);
    });
    Add("race", "The ball rolls (input reaches it)", "Race::BallPosition", function() {
        double x, y, z;
        if (step == 0)
        {
            Race::BallPosition(x, y, z);
            d1 = x;
            d2 = y;
            d3 = z;
            Console::Run("post 87 1500");
            step = 1;
            return WAIT;
        }
        if (Elapsed() < 2.5)
            return WAIT;
        Race::BallPosition(x, y, z);
        double moved = Math::sqrt((x - d1) * (x - d1) + (y - d2) * (y - d2) + (z - d3) * (z - d3));
        return Is(moved > 100, "Race::BallPosition: the ball moved " + int(moved) + " cm with W held 1.5 s");
    }, 8);
    Add("race", "Race restart", "Race::Restarts", function() {
        // A restart from the beginning, through the game's own ManuallyRestartBall (what R does before the first
        // checkpoint). Measured 2026-09-30: a Backspace posted to the window never reaches the game's restart (the
        // controller's restart gates all pass; the key is taken before its input action), and this test used to pass
        // only because the ball, still rolling from the test before, fell off the track about 10 s later. A fall isn't a
        // restart (Race::Falls), so that no longer counts.
        if (step == 0)
        {
            id1 = Race::Restarts();
            Console::Run("call BP_MyPlayerController_C ManuallyRestartBall");
            step = 1;
        }
        if (Race::Restarts() == id1 + 1)
            return PASS;
        return Elapsed() > 5 ? "Race::Restarts " + Race::Restarts() + " after a restart, want " + (id1 + 1) : WAIT;
    }, 8);
    Add("race", "Race checkpoints, respawns and falls", "Race::CheckpointCount,Race::CheckpointPosition,Race::CurrentCheckpoint,Race::Respawns,Race::Falls", function() {
        // Measured on Leth Trial 01 (9 checkpoint strips): the ball put in checkpoint 0's trigger (the host's
        // "checkpoints" test command logs where it is) makes it current, R respawns there, and a fall 300 m off to
        // the side is counted as a fall and not as an R respawn.
        if (step == 0)
        {
            if (!Race::IsActive())
                return Elapsed() > 15 ? "the race isn't running after the restart" : WAIT;     // R counts only while racing
            int n = Race::CheckpointCount();
            if (n <= 0)
                return "Race::CheckpointCount " + n + " on " + Race::TrackKey();
            double x, y, z;
            for (int i = 0; i < n; i++)
                if (!Race::CheckpointPosition(i, x, y, z))
                    return "Race::CheckpointPosition(" + i + ") false";
            if (Race::CheckpointPosition(n, x, y, z))
                return "Race::CheckpointPosition(" + n + ") true, past the last one";
            if (Race::CurrentCheckpoint() != -1)
                return "Race::CurrentCheckpoint " + Race::CurrentCheckpoint() + " right after a restart, want -1";
            Console::Run("checkpoints");
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            // "  0 at <x> <y> <z>, trigger <x> <y> <z>"
            string line = LogLineSince("  0 at ");
            int at = line.findFirst("trigger ");
            if (at < 0)
                return Elapsed() > 5 ? "no trigger for checkpoint 0 in the checkpoints command's lines" : WAIT;
            array<string>@ parts = line.substr(at + 8).split(" ");
            if (parts.length() < 3)
                return "can't read the trigger in: " + line;
            d1 = parseFloat(parts[0]);
            d2 = parseFloat(parts[1]);
            d3 = parseFloat(parts[2]);
            id1 = Race::Respawns();
            id2 = Race::Falls();
            Console::Run("teleport " + d1 + " " + d2 + " " + d3);
            step = 2;
            t0 = Host::Time();
            return WAIT;
        }
        if (step == 2)
        {
            if (Race::CurrentCheckpoint() != 0)
                return Elapsed() > 4 ? "Race::CurrentCheckpoint " + Race::CurrentCheckpoint() + " with the ball in checkpoint 0's trigger" : WAIT;
            Console::Run("post 82 200");
            step = 3;
            t0 = Host::Time();
            return WAIT;
        }
        if (step == 3)
        {
            if (Race::Respawns() != id1 + 1)
                return Elapsed() > 5 ? "Race::Respawns " + Race::Respawns() + " after R at checkpoint 0, want " + (id1 + 1) : WAIT;
            Console::Run("teleport " + (d1 + 30000) + " " + d2 + " " + d3);
            step = 4;
            t0 = Host::Time();
            return WAIT;
        }
        if (Race::Falls() != id2 + 1)
            return Elapsed() > 10 ? "Race::Falls " + Race::Falls() + " after a fall, want " + (id2 + 1) : WAIT;
        if (Elapsed() < 4)
            return WAIT;                        // the respawn comes a moment after the fall
        array<string> c = {Is(Race::Respawns() == id1 + 1, "Race::Respawns " + Race::Respawns() + " after a fall, want it unchanged at " + (id1 + 1)),
                           Is(Race::CurrentCheckpoint() == 0, "Race::CurrentCheckpoint " + Race::CurrentCheckpoint() + " after the fall's respawn, want 0")};
        return All(c);
    }, 45);
    Add("race", "Race save and load the ball", "Race::SaveBall,Race::LoadBall", function() {
        if (step == 0 && (!Race::IsActive() || Race::RunId() < 0))
            return WAIT;                            // the restarted run begins
        if (step == 0)
        {
            savedBall = Race::SaveBall();
            if (savedBall == "")
                return "Race::SaveBall empty";
            Race::BallPosition(d1, d2, d3);
            Console::Run("post 87 800");
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (Elapsed() < 1.5)
                return WAIT;
            if (!Race::LoadBall(savedBall, false))
                return "Race::LoadBall false";
            step = 2;
            return WAIT;
        }
        double x, y, z;
        Race::BallPosition(x, y, z);
        double off = Math::sqrt((x - d1) * (x - d1) + (y - d2) * (y - d2) + (z - d3) * (z - d3));
        return Is(off < 30, "Race::LoadBall: the ball is " + int(off) + " cm from where it was saved");
    }, 8);
    Add("race", "Race pause", "Race::SetPaused,Race::IsPaused", function() {
        if (step == 0)
        {
            if (!Race::SetPaused(true))
                return "Race::SetPaused(true) false";
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (!Race::IsPaused())
                return Elapsed() > 3 ? "Race::IsPaused false after Race::SetPaused(true)" : WAIT;
            Race::SetPaused(false);
            step = 2;
            return WAIT;
        }
        return Race::IsPaused() ? (Elapsed() > 5 ? "Race::IsPaused still true after Race::SetPaused(false)" : WAIT) : PASS;
    }, 8);

    Add("race", "Race practice", "Race::StartPractice,Race::IsPractice", function() {
        if (step == 0)
        {
            Race::StartPractice();
            step = 1;
            return WAIT;
        }
        return Race::IsPractice() ? PASS : WAIT;
    }, 8);
}

// --- workshop: a custom track -------------------------------------------------------------------------------------------------
void RegisterWorkshop()
{
    Add("workshop", "Tracks::OpenWorkshop opened a workshop track", "Tracks::OpenWorkshop,Tracks::OpenState,Race::IsCustomTrack,Race::TrackKey", function() {
        array<string> c = {Is(Race::IsCustomTrack(), "Race::IsCustomTrack false"), Is(Race::TrackKey().findFirst("custom:") == 0, "Race::TrackKey '" + Race::TrackKey() + "'"),
                           Is(Tracks::OpenState() == "open" || Tracks::OpenState() == "idle", "Tracks::OpenState '" + Tracks::OpenState() + "'")};
        return All(c);
    });
    Add("workshop", "Race::TrackImage of a workshop track", "Race::TrackImage", function() {
        // A workshop track's folder has "<track>_<author>.jpg" beside the map (measured).
        string image = Race::TrackImage();
        return Is(image.length() > 4 && image.substr(image.length() - 4) == ".jpg", "Race::TrackImage '" + image + "', want the .jpg beside the map");
    });
    Add("workshop", "Ghosts of the track on screen", "Ghosts::Load", function() {
        if (step == 0)
        {
            if (!Ghosts::Load("", 3))
                return Elapsed() > 30 ? "Ghosts::Load(\"\") answered false for 30 s" : WAIT;
            step = 1;
            return WAIT;
        }
        string state = Ghosts::State();
        if (state.findFirst("error") == 0)
            return "State " + state;
        return state == "ready" ? PASS : WAIT;
    }, 60);
}

// --- editor ---------------------------------------------------------------------------------------------------------------------
int piece = -1;
int choice = -1;

void RegisterEditor()
{
    Add("editor", "Editor open with pieces", "Editor::IsOpen,Editor::Pieces,Editor::PieceClass,Editor::MapName,Editor::IsTesting,Editor::Typing,Editor::Placed", function() {
        array<int>@ pieces = Editor::Pieces();
        piece = -1;
        for (uint i = 0; i < pieces.length() && piece < 0; i++)
            if (Editor::PieceClass(pieces[i]).findFirst("Track") >= 0 || Editor::PieceClass(pieces[i]).findFirst("Ramp") >= 0)
                piece = pieces[i];
        if (piece < 0 && pieces.length() > 0)
            piece = pieces[0];
        array<string> c = {Is(Editor::IsOpen(), "Editor::IsOpen false"), Is(pieces.length() > 0, "Editor::Pieces empty"), Is(Editor::PieceClass(piece).findFirst("_C") > 0, "Editor::PieceClass '" + Editor::PieceClass(piece) + "'"),
                           Is(Editor::MapName() != "", "Editor::MapName empty for a saved map"), Is(!Editor::IsTesting(), "Editor::IsTesting true while editing"),
                           Is(!Editor::Typing(), "Editor::Typing true with no text box focused"), Is(Editor::Placed().length() == 0, "Editor::Placed not empty with nothing placed")};
        return All(c);
    });
    Add("editor", "Editor asset operations reject missing assets", "Editor::SpawnMesh,Editor::SpawnAsset,Editor::SetMesh,Editor::SetMaterial,Editor::DescribeMesh,Editor::DescribeAsset,Editor::RepairMeshes", function() {
        string action = Editor::SpawnAsset("");
        array<string> c = {Is(Editor::SpawnMesh("") < 0, "Editor::SpawnMesh accepted an empty path"),
                           Is(action != "", "Editor::SpawnAsset returned no status for an empty path"),
                           Is(!Editor::SetMesh(piece, ""), "Editor::SetMesh accepted an empty path"),
                           Is(!Editor::SetMaterial(piece, ""), "Editor::SetMaterial accepted an empty path"),
                           Is(Editor::DescribeMesh("") == "", "Editor::DescribeMesh resolved an empty path"),
                           Is(Editor::DescribeAsset("") == "", "Editor::DescribeAsset resolved an empty path"),
                           Is(Editor::RepairMeshes() >= 0, "Editor::RepairMeshes returned a negative count")};
        return All(c);
    });
    Add("editor", "Editor reflected properties are read-only", "Editor::InspectProperties,Editor::InspectPropertiesPage,Editor::InspectPropertyChildren,Editor::PropertyObjectName", function() {
        string all = Editor::InspectProperties(piece);
        string page = Editor::InspectPropertiesPage(piece, "", 0, 4);
        array<string> c = {Is(all != "", "Editor::InspectProperties returned no fields"),
                           Is(page != "", "Editor::InspectPropertiesPage returned no fields"),
                           Is(all.findFirst("\t1\t") < 0 && all.findFirst("read-only inspection") >= 0, "Editor::InspectProperties advertised an editable field"),
                           Is(Editor::InspectProperties(-1) == "", "Editor::InspectProperties accepted a non-piece id"),
                           Is(Editor::InspectPropertiesPage(-1, "", 0, 4) == "", "Editor::InspectPropertiesPage accepted a non-piece id"),
                           Is(Editor::InspectPropertyChildren(piece, "Actor.__ApiTestsMissing", 4) == "", "Editor::InspectPropertyChildren returned fields for a missing path"),
                           Is(Editor::InspectPropertyChildren(-1, "Actor.Location", 4) == "", "Editor::InspectPropertyChildren accepted a non-piece id"),
                           Is(Editor::PropertyObjectName(-1) == "", "Editor::PropertyObjectName accepted a non-piece id"),
                           Is(Editor::PropertyObjectName(piece) != "", "Editor::PropertyObjectName returned an empty name")};
        return All(c);
    });
    Add("editor", "Editor move and turn a piece", "Editor::GetLocation,Editor::SetLocation,Editor::GetRotation,Editor::SetRotation", function() {
        double x, y, z, p, w, r;
        if (!Editor::GetLocation(piece, x, y, z) || !Editor::GetRotation(piece, p, w, r))
            return "Editor::GetLocation/Editor::GetRotation false";
        if (!Editor::SetLocation(piece, x + 10, y, z) || !Editor::SetRotation(piece, p, w + 15, r))
            return "Editor::SetLocation/Editor::SetRotation false";
        double x2, y2, z2, p2, w2, r2;
        Editor::GetLocation(piece, x2, y2, z2);
        Editor::GetRotation(piece, p2, w2, r2);
        Editor::SetLocation(piece, x, y, z);
        Editor::SetRotation(piece, p, w, r);
        array<string> c = {Near(x2, x + 10, "Editor::SetLocation: x after it", 0.01), Near(Math::abs(Math::sin((w2 - w - 15) * 3.14159265 / 180)), 0, "Editor::SetRotation: yaw after it", 0.001),
                           Is(!Editor::SetLocation(-5, 0, 0, 0), "Editor::SetLocation of a missing piece true")};
        return All(c);
    });
    Add("editor", "Editor selection", "Editor::Select,Editor::Selection,Editor::SetOutline,Editor::ScreenPosition,Editor::ViewForward", function() {
        array<int> one = {piece};
        Editor::Select(one);
        array<int>@ sel = Editor::Selection();
        double fx, fy, fz;
        Editor::ViewForward(fx, fy, fz);
        float sx, sy;
        Editor::ScreenPosition(piece, sx, sy);
        array<int> none;
        bool outline = Editor::SetOutline(piece, true) && Editor::SetOutline(piece, false);
        Editor::Select(none);
        array<string> c = {Is(sel.length() == 1 && sel[0] == piece, "Editor::Selection after Editor::Select: " + sel.length()), Is(Editor::Selection().length() == 0, "Editor::Selection not empty after Editor::Select([])"),
                           Near(fx * fx + fy * fy + fz * fz, 1, "Editor::ViewForward length", 0.001), Is(outline, "Editor::SetOutline false")};
        return All(c);
    });
    Add("editor", "Editor duplicate and rotate", "Editor::DuplicateSelection,Editor::RotatePieces", function() {
        array<int> one = {piece};
        Editor::Select(one);
        array<int>@ copies = Editor::DuplicateSelection();
        if (copies.length() != 1)
            return "Editor::DuplicateSelection made " + copies.length();
        double x, y, z, p, w, r;
        Editor::GetLocation(copies[0], x, y, z);
        Editor::GetRotation(copies[0], p, w, r);
        Editor::RotatePieces(copies, x, y, z, 0, 0, 90);
        double p2, w2, r2;
        Editor::GetRotation(copies[0], p2, w2, r2);
        Editor::SetLocation(copies[0], x, y, z + 5000);            // out of the way (the map is never saved)
        array<int> none;
        Editor::Select(none);
        return Near(Math::abs(Math::sin((w2 - w - 90) * 3.14159265 / 180)), 0, "Editor::RotatePieces: yaw after 90 about z", 0.001);
    });
    Add("editor", "Editor modes", "Editor::SetTabCycling,Editor::SetRotateAroundCenter,Editor::SetRotateMode,Editor::GetRotateMode", function() {
        Editor::RotateMode before = Editor::GetRotateMode();
        Editor::SetRotateMode(Editor::RotateMirrored);
        bool mirrored = Editor::GetRotateMode() == Editor::RotateMirrored;
        Editor::SetRotateAroundCenter(true);
        bool center = Editor::GetRotateMode() == Editor::RotateAroundCenter;
        Editor::SetRotateMode(before);
        Editor::SetTabCycling(true);
        Editor::SetTabCycling(false);
        array<string> c = {Is(mirrored, "Editor::GetRotateMode after Editor::SetRotateMode(Mirrored)"), Is(center, "Editor::GetRotateMode after Editor::SetRotateAroundCenter(true)")};
        return All(c);
    });
    Add("editor", "Editor toolbar and key list", "Editor::AddToolbarChoice,Editor::ToolbarChoice,Editor::SetToolbarChoice,Editor::AddHotkey", function() {
        if (step == 0)
        {
            array<string> options = {"api one", "api two"};
            choice = Editor::AddToolbarChoice("/Game/Art/UI/Textures/Editor/t_rotateIcon.t_rotateIcon", options, 0);
            if (choice < 0)
                return "Editor::AddToolbarChoice " + choice;
            Editor::AddHotkey("/Game/Art/UI/Textures/KeyboardMouse/keyboard_t.keyboard_t", "api key row");
            step = 1;
            return WAIT;
        }
        if (step == 1)
        {
            if (!LogSince("hotkey row 'api key row' added"))
                return Elapsed() > 5 ? "Editor::AddHotkey: the key row was not added" : WAIT;
            Editor::SetToolbarChoice(choice, 1);
            step = 2;
            return WAIT;
        }
        return Is(Editor::ToolbarChoice(choice) == 1, "Editor::ToolbarChoice " + Editor::ToolbarChoice(choice) + " after Editor::SetToolbarChoice(1)");
    }, 10);
    Add("editor", "Editor piece budget", "Editor::BudgetLimit,Editor::BudgetUsed,Editor::SetBudgetLimit", function() {
        int own = Editor::BudgetLimit();
        int used = Editor::BudgetUsed();
        array<string> c = {Is(own > 0, "Editor::BudgetLimit " + own),
                           Is(used > 0 && used <= int(Editor::Pieces().length()), "Editor::BudgetUsed " + used + " with " + Editor::Pieces().length() + " pieces"),
                           Is(Editor::SetBudgetLimit(own + 1234) && Editor::BudgetLimit() == own + 1234, "Editor::SetBudgetLimit: limit " + Editor::BudgetLimit() + " after setting " + (own + 1234))};
        Editor::SetBudgetLimit(0);
        c.insertLast(Is(Editor::BudgetLimit() == own, "Editor::SetBudgetLimit(0): limit " + Editor::BudgetLimit() + ", want the game's " + own));
        return All(c);
    });
    Add("editor", "Editor clicks", "Editor::NextClick", function() {
        int p, flags;
        bool was;
        while (Editor::NextClick(p, flags, was)) {}
        return "SKIP: a click on the world needs the real mouse (the pick traces from the cursor)";
    });
    Add("editortest", "Editor test run", "Editor::IsTesting,Race::BallPosition", function() {
        double x, y, z;
        array<string> c = {Is(Editor::IsTesting(), "Editor::IsTesting false in a test run"), Is(Race::BallPosition(x, y, z), "Race::BallPosition false in a test run")};
        return All(c);
    });
}
