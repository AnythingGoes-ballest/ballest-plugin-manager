// The smallest plugin: logs once when loaded and once a minute after that.

float elapsed = 0;

void Main()
{
    Log::Info("hello from a plugin");
}

void Update(float dt)
{
    elapsed += dt;
    if (elapsed >= 60)
    {
        elapsed = 0;
        Log::Info("still here");
    }
}
