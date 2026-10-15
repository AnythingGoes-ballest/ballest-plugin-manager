# Custom cosmetics

Plugins can add their own balls, hats and goal explosions to the game's Customize page. Each appears in a **custom**
section at the bottom of its tab, and choosing it works like choosing one of the game's.

The easy way is to depend on [Cosmetic Kit](https://github.com/AnythingGoes-ballest/ballest-cosmetic-kit), which also
remembers what the player wears. [Example Cosmetics](https://github.com/AnythingGoes-ballest/ballest-example-cosmetics)
is a complete example: a smiley ball, a morph ball with raised seams and glowing lights, a meatball with a spinning
saw, a T-rex and a skeleton running inside clear balls (3D model files), a fruit basket hat, a baseball cap and a
confetti goal explosion.

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

The Customize page has two buttons beside the cosmetics panel, **local** and **public**, each showing the ball and hat
it stands for:

- **Local** is worn on the player's own ball only: the menu ball and the ball they race with. Any cosmetic can be
  picked here, the game's or a custom one, for balls, hats and goal explosions. Picking the public one again makes
  local match public. The page opens in local mode.
- **Public** is the game's own choice: saved to the profile, and what other players see of the player: their ghost on
  hiscores and replays, and their ball on multiplayer maps such as the Tower. Only the game's cosmetics can be picked
  here. To change how others see you, switch to public first.

Custom cosmetics are always local, so the section of custom ones shows in local mode only. That way removing a plugin
never leaves the profile pointing at something that no longer exists.

## Ball textures

A ball's texture wraps around the ball: left to right goes once around, top to bottom goes from pole to pole. Make it
twice as wide as it is tall (1024x512). Something drawn flat is stretched near the poles, so draw it the way it should
look on the ball: Example Cosmetics' `make_images.py` projects its smiley and samples its textures on the sphere.

With no image (`""`) the ball is clear glass, so a model can sit inside it. A `bowl` of tinted glass in the model makes
half of the ball coloured, like the balls in Super Monkey Ball.
[Monkey Balls](https://github.com/AnythingGoes-ballest/ballest-monkey-balls) puts a running monkey in each.

## Models

A model is a text file of simple shapes, built into 3D meshes when the ball appears. On a ball it's centred on the
ball and rolls with it; on a hat it stands on the top of the ball. One statement per line; `#` at the start of a
line, or `# `, starts a comment. Lengths are in cm and the ball's radius is 50. Angles are in degrees.

```
material <name> plastic|metal|glow #rrggbb [rough=0.5] [bright=5]
material <name> glass [#rrggbb] [opacity=0.2] [ior=1.5] [reflect=1] [rough=0.05]
material <name> game <material asset path>
material <name> asset /Game/Mods/<mod>/<material>
set <material> "<parameter>" <value>
animate <material> "<parameter>" <from> <to> [period=4] [wave=sine|triangle|saw|square] [phase=0] [run=0]
race <material> "<parameter>" <factor>
tempo [rate=1] [run=0] [max=] [calm=1] [full=1]
ball hidden
group <name> [spin=x|y|z] [speed=<degrees a second>] [travel|facing] [on=<group>] [pivot=x,y,z]
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
| `bowl` | `r=` `wall=` | centred: the lower half of a sphere's shell, open at the top |
| `spiral` | `r=` `inner=` `turns=` `thick=` | centred, flat: a tube coiled from radius `r` in to `inner` |

- **Materials** come first. `plastic` and `metal` are solid colours (`rough` from 0, shiny, to 1, matte); `glow`
  lights up (`bright`); `glass` is the game's see-through glass, tinted with the colour (clear if none is given),
  `opacity` from 0 (invisible) to 1. Clear and tinted glass both show in front of the stadium water (host 0.14.0
  and newer: the host moves the game's tinted glass into the water's drawing pass). Colours show as given: the
  game's own rim tint and concrete grain are turned off.
- **Refracting glass** (host 0.23.2 and newer): give a `glass` material an `ior` (index of refraction: 1 is air and
  doesn't bend, glass is about 1.45-1.5, diamond 2.4) or a `reflect` (0 to 1, how strongly it reflects) and it
  becomes real glass. It bends what is behind it, like a solid glass ball, and reflects its surroundings: while the
  ball wears it, the host keeps a live 360-degree picture of the track around the ball (every frame, simplified: no
  shadows or fog, nothing past 300 m; about 3 ms a frame on an RTX 4070 Ti, shared by all the glass on the ball). On the Customize page, with no track around
  it, it reflects a sky. `opacity` 0 is as clear as glass gets; higher values cloud it.
- **`ball hidden`** (host 0.23.2 and newer): for a ball with no image, the game's own clear ball isn't drawn, for a
  model that is the whole ball, such as a glass sphere around it (it would show as a second ball inside).
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

### The game's materials, and colours that move

Host 0.27.0 and newer. A model can use any of the game's own materials, set up with parameters of your own, pictures
included, and change numbers and colours every frame. That's how a ball gets a glossy surface, a glow from a picture,
and light that flows and shimmers under the surface.

- **`material <name> game <path>`** is one of the game's materials, such as
  `/Game/Art/DataAssets/Skins/LBall/CameraVisibility/M_LBall05_CameraLogo`. The path can leave out the `.Name` at the
  end.
- **`set <material> "<parameter>" <value>`** sets one of its parameters (names with spaces in quotes). The value is a
  number, a colour (`#rrggbb`, or `r,g,b` in linear values; `*k` after either makes it k times as bright, for glowing
  colours such as `#39ff14*4`), a picture of the plugin's next to the model (`.png` or `.jpg`, a path from the model's
  folder such as `../textures/glow.png`), or one of the game's textures (a path starting with `/`). `set` works on
  every material, the plain kinds too: it comes after the host's own settings. A material ignores parameters it
  doesn't have.
- **`animate <material> "<parameter>" <from> <to>`** moves a number or a colour between two values every frame:
  `period` seconds a cycle, `wave` its shape (`sine`, `triangle` and `square` go there and back, `saw` goes from one
  to the other and starts over), `phase` degrees into the cycle, and `run` makes it that much faster for every m/s of
  the ball's speed. It works on the racing ball too, where the game's camera fade puts copies of the materials on.
- **`race <material> "<parameter>" <factor>`** multiplies that parameter, as set or animated, on every ball but the
  Customize page's (in races, the editor and replays; a colour's brightness for a colour). Daylight on a track washes
  glow out that looks right on the dark Customize page: `race crystal "EmissiveColor" 1.8`.
- **`material <name> asset /Game/Mods/<mod>/<material>`** is a material of the plugin's own: cooked in Unreal Engine
  5.8.3 and shipped in the plugin's `assets/` folder as a container (see
  [Custom cooked assets](../custom-assets-recipe.md)). `<mod>` is the plugin's id with `-` as `_`. It works like
  `game`, with `set`, `animate` and `race`. Only materials and material instances are used, and only from containers that
  hold nothing outside the plugin's own `/Game/Mods/<mod>/`.
- **`group <name> facing`** turns the group's +x toward the camera every frame. A disc across the ball's middle with a
  soft glow picture then shows only round the ball (the ball hides the rest of it): a glow halo.

Parameter names come from the game's materials: `matparams <material path>` in the console lists what an instance
sets, and the host's `dumptypes` mappings let tools such as FModel read the rest. Materials that work well on balls
(measured in game):

| Material | What it gives | Parameters |
|---|---|---|
| `/Game/Art/DataAssets/Skins/LBall/CameraVisibility/M_LBall05_CameraLogo` | A shiny surface with pictures: colour, glow, normal and shine maps, laid on by the model's UVs | `DiffuseColor`, `DiffuseColorMap` (+`Weight`), `EmissiveColor` (it multiplies the glow), `EmissiveColorMap` (+`Weight`), `NormalMap` (+`Weight`), `SpecularColor`, `Shininess` (higher is sharper) |
| `/Game/Packs/Vefects/Stylized_Galaxy_Shader/Galaxy/Materials/M_VFX_Lush_Galaxy_Shader` | A glowing deep space with twinkling stars that drift by themselves and a glowing rim; it stays still as the ball rolls, like something deep inside | `Galaxy Tint`, `Galaxy Emission Intensity`, `Galaxy Hue Shift`, `Stars Color 01`/`02`, `Stars Emission Intensity`, `Stars Speed X`/`Y`, `Fresnel Color`, `Fresnel Emission Intensity`, `Fresnel Power`, `Roughness`, `Specular` |
| `/Game/Packs/Vefects/Anime_VFX/Shared/Materials/Parents/M_VFX_DissolveAdd` | A see-through glowing layer whose pictures pan by themselves | `Main_Tex`, `GradientMap_Tex` (its colours, along the picture's width), `Brightness`, `MainTex_Speed_X`/`Y`. Give your picture to `Color_Tex`, `GradientShape_Tex` and `Dissolve_Tex` too, or the whole layer fogs |

```
# a glossy black ball with a green glow from a picture, turning slowly and breathing
ball hidden
material liquid game /Game/Art/DataAssets/Skins/LBall/CameraVisibility/M_LBall05_CameraLogo
set liquid "DiffuseColor" #030405
set liquid "DiffuseColorMapWeight" 0
set liquid "EmissiveColorMap" ../textures/glow.png
set liquid "EmissiveColorMapWeight" 1
set liquid "SpecularColor" #ffffff
set liquid "Shininess" 400
animate liquid "EmissiveColor" #ffffff*3 #ffffff*5.5 period=3.5
group swirl spin=z speed=14
mesh sphere.glb size=95 at=0,0,-47.5 material=liquid
```

```
# a soft glow round the ball: a disc (radius 80, facing +x) whose picture fades out from the ball's edge
material halo game /Game/Packs/Vefects/Anime_VFX/Shared/Materials/Parents/M_VFX_DissolveAdd
set halo "Main_Tex" ../textures/halo.png
set halo "Color_Tex" ../textures/halo.png
set halo "GradientShape_Tex" ../textures/halo.png
set halo "Dissolve_Tex" ../textures/halo.png
set halo "GradientMap_Tex" ../textures/halo_colour.png
group halo facing
mesh halo.glb at=0,0,-80 material=halo
```

A shiny surface also mirrors the track: on the start pad a mirror-shiny ball shows a band of the pad's yellow. A
lower `SpecularColor` (`#404040`) with a high `Shininess` keeps a sharp highlight without the band (measured).

Layers: a smaller sphere inside a larger one only shows where the larger one has gaps (a mesh with holes, such as
plates or shards) or is see-through (glass, or the DissolveAdd layer). Pictures load as sRGB textures. The host keeps a
picture per file path for the session, so after changing one, start the game again to see it.
[Elemental Balls](https://github.com/AnythingGoes-ballest/ballest-elemental-balls) is three balls built this way.

## 3D model files

Models made in Blender or another 3D tool can be used as they are, with their colours, textures and animations
(host 0.16.0 and newer). Export one of:

- **glTF 2.0**: `.glb` (one file), or `.gltf` with its `.bin` and images next to it. Blender: File > Export > glTF 2.0.
  Leave mesh compression (Draco) off.
- **OBJ**: `.obj` with its `.mtl` (and any image the `.mtl` names) next to it. No animations.

FBX isn't read: export glTF instead. Keep models light: every frame of an animation is a mesh of its own (a few
thousand triangles is plenty; the examples are 1,800 and 5,300).

A model file can be the whole model: pass it as `model` (`f + "models/lamp.glb"`). On a ball it stands on the bottom of
the ball, 80 cm across; on a hat it stands on the top of the ball, 40 cm across. Or place it in a model's text file with a `mesh` line, alongside shapes and groups:

```
mesh <file> [size=<cm>] [at=x,y,z] [rot=pitch,yaw,roll] [scale=x,y,z] [material=<name>]
     [anim=<name>] [rate=1] [run=0] [idle=<name>] [frames=24]
```

- `<file>` is next to the text file (in quotes if its name has spaces).
- `size` makes its longest side that many cm. It stands on `at`: the bottom of its bounds, centred there, after `rot`
  has turned it. The file's front (Blender's -Y, glTF's +Z) faces the ball's forward direction, which on a hat is the
  way the brim of a cap should point.
- Its **colours** come from the file: a base colour (plastic, or metal when metallic), a base colour texture, an
  emissive colour (glows), or an alpha below 1 (tinted glass). `material=` uses one of the text file's materials
  for all of it instead.
- **Glass** from Blender works as it does there (host 0.23.2 and newer): a Principled BSDF with **Transmission**
  above 0 becomes [refracting glass](#models), bending what is behind it by its **IOR** and reflecting the real track
  around the ball. Its base colour tints it, its roughness and specular are kept, and the more it transmits, the
  clearer it is. Blender's glTF exporter writes all of this (KHR_materials_transmission and _ior) without any option.
- `anim` plays one of the file's animations (its name, or part of it): `rate` times its own speed while the ball is
  still, plus `run` more for every m/s of the ball's speed. `idle` plays another animation while the ball is still
  (below 0.5 m/s). Each is baked into `frames` poses when the plugin loads (skinned characters too).
- Put the `mesh` in a `travel` group to keep it upright and facing where the ball goes, like a runner.

```
# a T-rex running inside a clear ball (AddBall with no image)
group dino travel
mesh trex.glb size=85 at=0,0,-44 anim=run rate=0.6 run=0.1 idle=idle
```

```
# a cap from a file that has it tilted and facing back: turned upright, brim forward
mesh cap.glb size=36 rot=-10.7,-141.9,8.5
```

If a file can't be read, the log says why (a missing `.bin`, compression, an animation that isn't there, with the
names of those that are).

## Extras (arms, bounce)

Extras are new kinds of cosmetics, each in a slot of its own, worn with the ball and hat (host 0.19.0 and newer). The
first is **arms**, added through [Cosmetic Kit Plus](https://github.com/AnythingGoes-ballest/ballest-cosmetic-kit-plus).
Each slot gets a tab of its own on the Customize page, after balls, hats and bfx (shown in local mode), with a
**none** tile first.

An extra is a model, built on the ball like a ball's model: the ball's middle is the origin, its radius is 50, +x is
forward and +y the ball's right. Put it in a `travel` group so it stays upright and faces where the ball goes, and give
it swinging groups or an animation to move as the ball rolls. [Example Arms](https://github.com/AnythingGoes-ballest/ballest-example-arms)
has arms built from shapes and arms cut out of animated characters.

```
# a pair of stubby arms, swinging as the ball rolls
material skin plastic #e0a070
tempo rate=1 run=0.3 max=6
group body travel
group armL on=body pivot=0,-52,5 swing=y angle=35
capsule skin r=5 len=20 at=0,-54,5 rot=180,0,0
group armR on=body pivot=0,52,5 swing=y angle=35 phase=180
capsule skin r=5 len=20 at=0,54,5 rot=180,0,0
```

The second is **bounce**: an effect when the ball lands or hits a wall (host 0.20.0 and newer). A bounce tile has no
model; the plugin that adds it plays the effect while it's worn. Each frame it reads the bounces with
[Race::NextBounce](../reference/api/race.md) (where, which way the surface faces, and how hard, from 0 to 1) and plays
the game's particle effects there (`Draw::Effect`), sounds (`Draw::Sound`: the game's, or `.wav` files of its own),
models of its own (`Draw::Model`) and a
camera shake. [Example Bounce](https://github.com/AnythingGoes-ballest/ballest-example-bounce) has fifteen, each soft,
medium or hard by the hit.

```angelscript
import bool AddBounce(const string &in, const string &in, const string &in) from "cosmetic-kit-plus";

void Update(float dt)
{
    double s, x, y, z, nx, ny, nz;
    bool ground;
    bool worn = Cosmetics::EquippedExtra("bounce") == "my-bounce.boom";
    while (Race::NextBounce(s, x, y, z, nx, ny, nz, ground))
        if (worn)
            Draw::Effect("/Game/Art/NS_BallExplosion.NS_BallExplosion", x, y, z, 0.3 + s, nx, ny, nz);
}
```

To show an effect when it's picked, play it under the menu ball: while the Customize page is shown,
[Cosmetics::PreviewBall](../reference/api/cosmetics.md) gives the ball's middle, its radius and the way that faces the
camera (host 0.20.0 and newer).

## Goal explosions

A custom goal explosion is one of the game's at another size, or with another of the game's effects and sounds. It
plays at checkpoints and in the Customize preview. Custom effect files can't be loaded: the game only loads its own
packaged content.
