# Contributing to CyberCraft

Bug reports, fixes and testing in the game are all welcome. CyberCraft is early: a lot of it is
written but hasn't been run in Cyberpunk yet, so "I tried X and here's what happened" is as useful
as code.

## Before you start

- [docs/DESIGN.md](docs/DESIGN.md) is how the two halves fit together.
- [docs/CYBERCRAFT.md](docs/CYBERCRAFT.md) is what exists today, which game calls have been seen
  working and which haven't.
- [docs/ROADMAP.md](docs/ROADMAP.md) is what's next.

For anything bigger than a fix, open an issue first so we can agree on the approach before you
spend time on it.

## Setup

You need Visual Studio 2022 with C++, CMake 3.25+, Git, JDK 25 and Python 3 (for the test
stand-ins in `tools/`). To try a change in the game you also need Cyberpunk 2077 with
[RED4ext](https://github.com/wopss/RED4ext) and [Codeware](https://github.com/psiberx/cp2077-codeware).

```bat
git clone --recursive https://github.com/YoGoUrT20/cybercraft.git
cd cybercraft\red4ext
cmake --preset default
cmake --build --preset release

cd ..\fabric
gradlew build
```

Cloned without `--recursive`? Run `git submodule update --init --recursive` for RED4ext.SDK.

Set `CYBERCRAFT_DEPLOY_DIR` to `<game>\red4ext\plugins\CyberCraft` before `cmake --preset default`
and every plugin build is copied straight into the game. `install.bat` builds, packs and installs
everything in one go (see the README).

## Testing

- **Unit tests:** `gradlew test` in `fabric/`.
- **The Minecraft mod without the game:** `tools\fake_cyberpunk.py` stands in for the plugin.
  Start it, then `tools\launch_minecraft.bat` (or run `tools/test_mc.sh <out-dir>`, which does
  both and collects the logs).
- **In the game:** set `bDiagnostics = 1` under `[Debug]` in `CyberCraft.ini`; the plugin logs to
  `red4ext\logs\CyberCraft.log`. `tools\cet.ps1` runs Lua in the live game through the
  [Cyber Engine Tweaks](https://github.com/maximegmd/CyberEngineTweaks) dev console in
  `tools\cet-devconsole\`.

Say in the pull request how you tested it, and whether it was run in the game or not.

## Keeping the two halves in step

- **Protocol.** `protocol/cybercraft_protocol.h` is the shared memory layout and the source of
  truth. Mirror every offset and field change in
  `fabric/src/main/java/dev/cybercraft/link/Proto.java`, and bump `kVersion` and `Proto.VERSION`
  together.
- **Game calls.** A new call into Cyberpunk goes into the table in
  [docs/CYBERCRAFT.md](docs/CYBERCRAFT.md), marked unverified until someone has seen it work in the
  game.
- **Config keys.** A new `CyberCraft.ini` key is documented in
  [docs/CYBERCRAFT.md](docs/CYBERCRAFT.md#configuration), and in the README if players need it.
- **Scripts.** `red4ext/scripts/*.reds` hook the game's own classes: redscript won't let one mod's
  annotations touch another mod's classes. Check that a script compiles as described at the end of
  docs/CYBERCRAFT.md, against a copy of the script cache, never the game's own.

## Code style

Match the code around you. `.editorconfig` has the indentation: tabs in C++, Java, Lua and Gradle;
spaces in Python, PowerShell and redscript. The plugin builds with `/W4` and should stay
warning-free. Comments say why, not what.

In the protocol, `Cyber` names (`CyberState`, `kCyberInGame`) are the Cyberpunk plugin's side and
`Mc` names Minecraft's. The Fabric mod's classes that talk to Cyberpunk start with `Cyber` too
(`CyberLink`, `CyberCollision`).

## Commits and pull requests

Commit subjects are short, with a lowercase prefix: `add:`, `fix:`, `feat:`, `refactor:`,
`chore:`. For example `fix: hand drawn over the map`. One topic per pull request.

Before opening one:

- both halves build, and `gradlew test` passes
- the protocol version is bumped if the layout changed
- the docs say what changed (new keys, new game calls, new limitations)

## License

By contributing you agree that your work is released under the [MIT License](LICENSE).
