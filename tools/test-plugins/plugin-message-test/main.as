bool isHost = false;
bool created = false;
bool published = false;
bool limitsChecked = false;
bool passed = false;
bool failed = false;
double passedAt = 0;
double nextSearch = 0;
double nextSend = 0;
double createAfter = 0;

void Fail(const string &in reason)
{
    Log::Error("PLUGIN_MESSAGE_TEST FAIL " + reason);
    failed = true;
}

void Main()
{
    if (Together::Backend() != "test")
    {
        Fail("requires the sandbox lobby backend");
        return;
    }
    string ownName = Together::OwnName();
    int slotAt = ownName.findFirst("(slot ");
    int slot = slotAt >= 0 && slotAt + 6 < int(ownName.length()) ? int(ownName[slotAt + 6]) - 48 : 9;
    createAfter = Host::Time() + slot;
    Log::Info("PLUGIN_MESSAGE_TEST candidate slot=" + slot);
}

void Update(float dt)
{
    if (failed) return;
    const double now = Host::Time();
    if (passed && now - passedAt >= 5) return;
    const string state = Together::LobbyState();
    if (!isHost && state == "none" && !Together::Searching() && Together::FoundCount() > 0)
    {
        Together::JoinLobby(Together::FoundId(0));
        return;
    }
    if (!isHost && state == "none" && !Together::Searching() && now >= createAfter)
    {
        isHost = true;
        created = true;
        Log::Info("PLUGIN_MESSAGE_TEST role=host");
        Together::CreateLobby(Together::Code, 2);
        return;
    }
    if (isHost && state == "in lobby" && !published)
    {
        published = Together::SetLobbyData("ch", "PLUGIN_MESSAGE_TEST") &&
                    Together::SetLobbyData("ct", "1") && Together::SetLobbyJoinable(true);
        if (!published)
            Fail("host could not publish the test lobby");
        return;
    }
    if (!isHost && state == "none" && !Together::Searching() && now >= nextSearch)
    {
        Together::FindLobbies("PLUGIN_MESSAGE_TEST");
        nextSearch = now + 1;
        return;
    }
    if (state.findFirst("error") == 0)
    {
        Fail("lobby " + state);
        return;
    }
    if (state != "in lobby" || Together::LobbyMemberIds().length() != 2) return;
    if (!limitsChecked)
    {
        limitsChecked = true;
        string tooLong;
        for (uint i = 0; i < 769; ++i)
            tooLong += "x";
        if (Together::SendPluginMessage("") || Together::SendPluginMessage("line\nbreak") ||
            Together::SendPluginMessage(tooLong))
        {
            Fail("an invalid payload was accepted");
            return;
        }
    }
    if (now >= nextSend)
    {
        Together::SendPluginMessage("hello " + Together::OwnId());
        nextSend = now + 0.5;
    }
    string sender, message;
    while (Together::NextPluginMessage(sender, message))
    {
        if (sender == Together::OwnId()) continue;
        bool member = false;
        array<string>@ members = Together::LobbyMemberIds();
        for (uint i = 0; i < members.length(); ++i)
            member = member || members[i] == sender;
        if (!member || message != "hello " + sender)
        {
            Fail("sender or payload was not authenticated by the lobby");
            return;
        }
        if (!passed)
        {
            Log::Info("PLUGIN_MESSAGE_TEST PASS peer=" + sender);
            passed = true;
            passedAt = now;
        }
        return;
    }
}
