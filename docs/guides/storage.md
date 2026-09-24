# Saving data and timing

## Saving between launches

[Storage](../reference/api/storage.md) keeps text values by key, per plugin, across launches of the game. Numbers are
saved as text and parsed back:

```cpp
double seconds = 0;
int restarts = 0;

void Main()
{
    seconds = parseFloat(Storage::Get("seconds", "0"));
    restarts = int(parseInt(Storage::Get("restarts", "0")));
}

void Save()
{
    Storage::Set("seconds", formatFloat(seconds, "", 0, 3));
    Storage::Set("restarts", "" + restarts);
}
```

Each changed value is written to disk as soon as you set it, so don't call `Storage::Set` every frame for a value
that changes every frame. Remember that something changed, and save every few seconds:

```cpp
const double SAVE_EVERY = 5.0;
double lastSave = 0;
bool dirty = false;

void Update(float dt)
{
    if (Race::IsActive())
    {
        seconds += dt;
        dirty = true;
    }
    if (dirty && Host::Time() - lastSave > SAVE_EVERY)
    {
        Save();
        lastSave = Host::Time();
        dirty = false;
    }
}
```

Your plugin only ever sees its own values. They're kept in
`%LOCALAPPDATA%\Ballest\Saved\PluginManager\storage\<plugin id>.txt`, and survive removing and reinstalling the
plugin. Keys starting with `setting.` hold your [settings](settings.md), and `window.` keys hold where the player
dragged your windows, so use other names for your own values.

## Timing

`Update(float dt)` gets the length of the last frame, which is what you add up to count time. It's the right tool
for "how long has the player been racing":

```cpp
if (Race::IsActive())
    racing += dt;
```

`Host::Time()` is a clock in seconds since the game started. It keeps running through loading screens and menus.
Use it for "has it been five seconds since...":

```cpp
if (Host::Time() - shownAt > 3.0)
    toast.visible = false;
```

A frame after a long load can have a very large `dt`. If a hitch shouldn't count, clamp it:

```cpp
double step = dt > 1.0 ? 1.0 : dt;
```
