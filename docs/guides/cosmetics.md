# Custom cosmetics

Plugins can add their own balls, hats and goal explosions to the game's Customize page. Each appears in a **custom**
section at the bottom of its tab, and choosing it works like choosing one of the game's.

The easy way is to depend on [Cosmetic Kit](https://github.com/AnythingGoes-ballest/ballest-cosmetic-kit), which also
remembers what the player wears. [Example Cosmetics](https://github.com/AnythingGoes-ballest/ballest-example-cosmetics)
is a complete example: a smiley ball, a morph ball with raised seams and glowing lights, a meatball with a spinning
saw, a fruit basket hat and a confetti goal explosion.

```toml
# info.toml
[meta]
dependencies = ["cosmetic-kit"]
```

```angelscript
import bool AddBall(const string &in, const string &in, const string &in, const string &in, const string &in) from "cosmetic-kit";

void Main()
{
    string f = Plugins::Folder();
    AddBall("my-balls.planet", "planet", f + "planet.png", f + "planet_tile.png", f + "rings.txt");
}
```

The functions are the same as [Cosmetics](../reference/api/cosmetics.md) in the API.

## Who sees them

Custom cosmetics are worn on the player's own ball only: the menu ball and the ball they race with. The game's save,
and what other players see, keep the last cosmetic chosen from the game's own. That way removing a plugin never leaves
the profile pointing at something that no longer exists.

## Ball textures

A ball's texture wraps around the ball: left to right goes once around, top to bottom goes from pole to pole. Make it
twice as wide as it is tall (1024x512). Something drawn flat is stretched near the poles, so draw it the way it should
look on the ball: Example Cosmetics' `make_images.py` projects its smiley and samples its textures on the sphere.

With no image (`""`) the ball is clear, made of the game's snow globe glass, so a model can sit inside it.

## Models

A model is a text file of simple shapes, built into 3D meshes when the ball appears. On a ball it's centred on the
ball and rolls with it; on a hat it stands on the top of the ball. One statement per line; `#` at the start of a
line, or `# `, starts a comment. Lengths are in cm and the ball's radius is 50. Angles are in degrees.

```
material <name> plastic|metal|glow #rrggbb [rough=0.5] [bright=5]
material <name> glass [rim=1] [highlight=1]
tempo [rate=1] [run=0] [max=] [calm=1] [full=1]
group <name> [spin=x|y|z] [speed=<degrees a second>] [travel] [on=<group>] [pivot=x,y,z]
      [swing=x|y|z angle=<degrees>] [bob=<cm>] [phase=<degrees>]
<shape> <material> <sizes> [at=x,y,z] [rot=pitch,yaw,roll] [scale=x,y,z]
```

| Shape | Sizes | Placed |
|---|---|---|
| `sphere` | `r=` | centred on `at` |
| `box` | `size=x,y,z` | centred |
| `cylinder` | `r=` `h=` | standing on `at`, along z |
| `cone` | `r=` `top=` `h=` | standing |
| `capsule` | `r=` `len=` | standing |
| `disc` | `r=` `hole=` | centred, flat |
| `ring` | `r=` `thick=` `degrees=` | centred, flat around z (`degrees` for an arc, such as a handle) |
| `saw` | `r=` `teeth=` `depth=` `thick=` | centred, flat |
| `cup` | `r=` `top=` `h=` `wall=` | standing: a bowl open at the top |

- **Materials** come first. `plastic` and `metal` are solid colours (`rough` from 0, shiny, to 1, matte); `glow`
  lights up (`bright`); `glass` is the game's see-through snow globe glass, with its rim and highlight made stronger
  or weaker (`rim`, `highlight`: 1 is the game's own).
- **Groups** collect the parts after them. `spin` turns the group about an axis; `travel` keeps it level and turned
  the way the ball is going instead of rolling with the ball (a blade that stays upright, for example). Before the
  ball has moved, a travelling group faces away from the camera, and on the Customize page it faces the camera.
- **Moving parts**, such as a character's arms and legs:
    - `on=<group>` builds a group on an earlier group, so it moves with it (a leg on a body).
    - `pivot` is the point it turns about (a hip), in the same coordinates as everything else.
    - `swing` rocks it to and fro about an axis through the pivot, `angle` degrees each way.
    - `bob` lifts it by that many cm and lets it down, twice a swing (once a step).
    - `phase` puts a group's swing and bob later in the cycle (180: opposite, like the other leg).
  A travelling group can't swing or bob itself: make an empty travelling group and build the moving ones on it.
- **Tempo** sets the pace of every swing and bob:
    - `rate`: swings a second when the ball is still.
    - `run`: more swings a second for every m/s of the ball's speed, up to `max`.
    - `calm`: how much of the swing is left when the ball is still (0 to 1); swings grow to their full size at
      `full` m/s.

```
# a character running upright inside the ball
material fur plastic #8a5226
tempo rate=0.6 run=0.25 max=5 calm=0.3 full=10
group monkey travel
group body on=monkey bob=2.5
sphere fur r=12 at=0,0,-14
sphere fur r=19 at=0,0,10
group legL on=body pivot=0,-5.5,-22 swing=y angle=45
capsule fur r=4 len=4 at=0,-5.5,-22 rot=180,0,0
group legR on=body pivot=0,5.5,-22 swing=y angle=45 phase=180
capsule fur r=4 len=4 at=0,5.5,-22 rot=180,0,0
```

```
# a spinning ring of lights around the ball
material dark metal #303036
material light glow #40c0ff bright=15
group halo spin=z speed=120 travel
ring dark r=62 thick=4
sphere light r=4 at=62,0,0
sphere light r=4 at=-62,0,0
sphere light r=4 at=0,62,0
sphere light r=4 at=0,-62,0
```

If a model has a mistake, the cosmetic isn't added and the log says which line.

## Goal explosions

A custom goal explosion is one of the game's at another size, or with another of the game's effects and sounds. It
plays at checkpoints and in the Customize preview. Custom effect files can't be loaded: the game only loads its own
packaged content.
