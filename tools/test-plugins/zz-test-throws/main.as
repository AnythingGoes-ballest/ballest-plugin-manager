// Regression fixture: throws a script exception (null handle) on its tenth frame. The host must stop only this
// plugin.
int frames = 0;

void Update(float dt)
{
    if (++frames == 10)
    {
        UI::Panel@ none = null;
        none.Clear();
    }
}
