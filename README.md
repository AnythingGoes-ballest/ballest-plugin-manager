# Ballest plugin manager

## About

A plugin manager for **Ballest of Them All**, like Openplanet for Trackmania. It adds a **plugins** button to the
game's footer, where you can browse, search, install, update and remove plugins without leaving the game.

- Plugins are small scripts: timers, replay controls, track editor tools, and more.
- Search the installed plugins and the ones to install as you type: any word of a name finds it, even from the middle.
- Every download is checked against a SHA-256 hash before it's installed.
- A plugin that crashes or hangs is stopped on its own. The game keeps running.
- No UE4SS or other mod loader needed.

## Watch

[![Ballest of Them All Had No Mods… So I Built a Plugin System](https://img.youtube.com/vi/1d29QCz6rbk/maxresdefault.jpg)](https://www.youtube.com/watch?v=1d29QCz6rbk)

[Ballest of Them All Had No Mods… So I Built a Plugin System](https://www.youtube.com/watch?v=1d29QCz6rbk): what the
plugin manager is, how it came together and what it can do. More videos on my channel,
[AnythingGoes](https://www.youtube.com/@AnythingGoes-y9n).

The plugin manager is free. If you'd like to support it anyway, you can
[buy me a coffee](https://buymeacoffee.com/AnythingGoesYT). No pressure :)

## Install

You need [Git for Windows](https://git-scm.com/download/win), which comes with Git Bash. Close the game first.

Open **Git Bash** and run:

```bash
git clone https://github.com/AnythingGoes-ballest/ballest-plugin-manager.git
```

Downloads this repo, which contains the installer.

```bash
cd ballest-plugin-manager
```

```bash
./install.sh
```

Finds the game through Steam, downloads the latest release, checks it, and installs it.

Then start the game and click **plugins** in the footer at the bottom of the screen.

If the installer can't find the game, give it the folder:

```bash
./install.sh --game "C:\Program Files (x86)\Steam\steamapps\common\Ballest of Them All\Ballest\Binaries\Win64"
```

**Updating:** click **update plugin manager** in the game when it offers one.

## Uninstall

Close the game. In Git Bash, from the `ballest-plugin-manager` folder:

```bash
./uninstall.sh
```

Removes the plugin manager and every plugin from the game folder. Your plugin settings and saved data are kept, in
case you reinstall.

```bash
./uninstall.sh --purge
```

Does the same, and also deletes the saved plugin data.

Add `--dry-run` to either to see what would be removed without removing anything.

## Develop

**Writing plugins:** everything is in the **[Ballest plugin docs](https://anythinggoes-ballest.github.io/ballest-plugin-manager/)**:
a first-plugin tutorial, guides, and the full API with an example for every function.

**Working on the plugin manager itself:** building, testing and releasing are covered in
[Working on the host](https://anythinggoes-ballest.github.io/ballest-plugin-manager/host/).

## License

MIT (see [LICENSE](LICENSE)). AngelScript is under its own zlib license (`third_party/angelscript/LICENSE.txt`).

### The plugin manager's window

The footer's **plugins** button opens a window the host draws (`src/host/manager.*`): the plugins (installed and
browse), a **cosmetics** page (the registry's cosmetic plugins as packs, installed into `plugins\cosmetics`), settings
(general and advanced, then each plugin's own) and the console. Its **Theme** setting picks the look: **Ballest Night**
(the default), **Ballest Light**, **Lucid Lime**, or **Classic**, the old menu. With Cosmetic Kit on, the Customize page
gets the local/public strip, loadouts and slot tabs. The script API adds `Plugins::ShowManager`,
`Plugins::ManagerShown` and `Registry::Listed` (`docs/api-examples.txt`).
