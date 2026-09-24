# Ballest plugin manager

## About

A plugin manager for **Ballest of Them All**, like Openplanet for Trackmania. It adds a **plugins** button to the
game's footer, where you can browse, install, update and remove plugins without leaving the game.

- Plugins are small scripts: timers, replay controls, track editor tools, and more.
- Every download is checked against a SHA-256 hash before it's installed.
- A plugin that crashes or hangs is stopped on its own. The game keeps running.
- No UE4SS or other mod loader needed.

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
