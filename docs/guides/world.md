# Drawing in the world

Shapes in the world, a camera of your own, and a track's leaderboard runs: what a ghost viewer or a line-drawing tool
is built from. All of it needs host 0.14.0.

## Shapes

`Draw::Tube` sweeps a tube along points (x, y, z triples, in the game's centimetres), `Draw::Ball` makes a ball at the
origin to be moved with `Draw::Move`. Each returns an id; `Draw::Show` hides and shows it, `Draw::Remove` removes it,
`Draw::Clear` removes everything the plugin drew. Everything goes when the map changes.

```cpp
array<double> path = {0, 0, 100, 500, 0, 150, 1000, 200, 150};
int line = Draw::Tube(path, 5, 0.2f, 0.8f, 0.4f, false);          // radius, colour, glowing
int see = Draw::Tube(path, 5, 0.2f, 0.8f, 0.4f, false, 0.25f);    // see-through: 25% opaque tinted glass
int ball = Draw::Ball(25, 1, 0.5f, 0.2f, true);
Draw::Move(ball, 500, 0, 150);
```

A glowing shape's colour and brightness change in place with `Draw::Glow`, a see-through one's opacity with
`Draw::Fade`, so things can pulse and fade without being made again. Tinted glass is drawn in front of the stadium's
water like everything else.

For many short lines at once, `Draw::Segments` puts every start-and-end pair into one shape, so a thousand ticks cost
about as much as one tube. `Draw::Retube` gives an existing tube a new path in place, for the end of a trail that moves
every frame without making shapes. Both need host 0.25.0.

## A camera of your own

`Camera::Take` looks through a camera of the plugin's; `Camera::Set` places it (position, pitch, yaw, field of view)
and `Camera::Release` gives the game its view back. One plugin at a time has it. `Camera::Project` says where a point
in the world is on screen, for labels over things.

```cpp
Camera::Take();
Camera::Set(0, -2000, 1500, -30, 90, 90);
float sx, sy;
if (Camera::Project(0, 0, 100, sx, sy))
    label.SetPosition(sx, sy);
```

`Race::HideBall(true)` hides the player's own ball while you show the track. In a track editor test run,
`Race::FreezeBall(true)` also holds it still where it is (never in a run that can reach a leaderboard), and
`Camera::Sweep` tells a plugin camera how far it can go before it hits a wall. These two need host 0.25.0.

`Race::BallPosition` says where the ball being played is, in a race or a track editor test run (`Editor::IsTesting`
says when one is on), for drawing its path as it goes. Both need host 0.15.2.

## A track's runs

`Ghosts::Load` downloads the top of the leaderboard on screen (and the player's own run) from Steam; `Ghosts::State`
says how it's going and `Ghosts::Count` how many are here. Each run's position at a moment of it comes from
`Ghosts::Position`, its player's camera from `Ghosts::View`, its checkpoints from `Ghosts::CheckpointOrder` and
`Ghosts::Splits`.

```cpp
Ghosts::Load("", 25);
// every frame
for (int i = 0; i < Ghosts::Count(); i++)
{
    double x, y, z;
    if (Ghosts::Position(i, playTime, x, y, z))
        Draw::Move(balls[i], x, y, z);
}
```

`Ghosts::PlayerBall` makes a run's own ball, in its player's skin and accessory, placed with `Ghosts::PlaceBall`.
For thousands of runs at once, a crowd (`Ghosts::CrowdCreate`, `CrowdMembers`, `CrowdSkins`, `CrowdTrails`,
`CrowdPlace`) draws them together, placed by the host in one call a frame.

Runs can also come from a file the plugin ships or makes itself (`Ghosts::LoadFile`, in the plugin's own folder; the
format is on the Ghosts page), and `Ghosts::PlayerBall(i, true)` shows a run's ball in its player's real skin instead
of the rival-ghost look. Crowd trails can glow or fade with age (`CrowdTrailStyle`), and a colour group can be dimmed to
glass (`CrowdGroupGlass`). These need host 0.25.0.

`Tracks::` lists the game's tracks and searches the workshop, and opens a track by its key or workshop id.
