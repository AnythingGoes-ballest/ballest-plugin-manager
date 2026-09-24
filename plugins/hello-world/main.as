// The smallest plugin: logs once when loaded, then a message every so often. Its two settings show how a plugin
// gets a settings page (footer plugins > open > installed > Hello World > settings).

[Setting name="Message" description="What it writes to the log"]
string Message = "still here";

[Setting name="Every" min=5 max=300 description="Seconds between messages"]
float Every = 60;

float elapsed = 0;

void Main()
{
    Log::Info("hello from a plugin");
}

// The player changed a setting: say so, and start counting again.
void OnSettingsChanged()
{
    Log::Info("now saying '" + Message + "' every " + int(Every) + " s");
    elapsed = 0;
}

void Update(float dt)
{
    elapsed += dt;
    if (elapsed >= Every)
    {
        elapsed = 0;
        Log::Info(Message);
    }
}
