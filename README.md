# Offline Quiet (native code)

Source of the native plugin in the Swofty Launcher addon **Offline Quiet** (`twilight.offline-quiet`) for F1 25.
It hides the EA sign-in popups (EA Login, Online Services Error, Chat Restricted) that modded offline play shows
on every boot, and answers them the way the player would.

- `src/offline_quiet.cpp`: the whole plugin (hooks the game's popup constructor and answers on the next frame).
- `build.sh`: builds `offline_quiet.dll` with LLVM clang for `x86_64-pc-windows-msvc`. Pass the path of the
  `templates/addon/native/include` folder of the [Swofty Launcher](https://github.com/Swofty-Developments/SwoftyLauncher) repo.

```bash
./build.sh /path/to/SwoftyLauncher/templates/addon/native/include
```

By Twilight Sparkle.
