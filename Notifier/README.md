# Media Countdowns Notifier

The tray program that belongs with [Media
Countdowns](../Manager/README.md). It sits
in the notification area and tells you when something has come out.

It exists because the main app isn't always open, and a countdown that reaches
zero while nobody is looking may as well not have. This runs quietly, checks
once a minute, and pops a Windows notification when a tile's date and time
arrive.

**It needs no keys and no configuration.** It doesn't talk to TMDB, IGDB,
TVmaze or any relay — it only reads the file the main app writes,
`%APPDATA%\MediaCountdowns\tiles.json`. Build it, run it, done.

---

## Building

Same toolchain as the main app: **Qt 6.7.3** (MinGW 64-bit), **CMake 3.21+**,
**Ninja**. Open `CMakeLists.txt` in Qt Creator and build, or:

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.7.3/mingw_64" -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

Then, so it can run outside Qt Creator:

```bash
C:/Qt/6.7.3/mingw_64/bin/windeployqt.exe build/MediaCountdownsNotifier.exe
```

It adds itself to the Windows Startup folder the first time it runs, so it
comes back after a reboot. Delete the shortcut from
`shell:startup` if you'd rather it didn't.

---

## Things that look like bugs and aren't

Worth knowing before you change something:

- **It notifies while the main app is open, on purpose.** The main app never
  fires a notification on its own, so if this one stood down whenever the app
  was running, anything releasing during that time would be announced by
  nobody.
- **Its air-time handling is simpler than the main app's, on purpose.** The
  main app resolves time zones and writes the finished time into `tiles.json`,
  precisely so this program doesn't have to. Re-deriving it here would be
  wrong.
- **Only one copy may run.** A second launch hands over to the first and exits.
  Without that guard both would notify and you'd get everything twice.
- **`src/tiledata.h` and `src/loopschedule.h` are trimmed copies of the main
  app's.** `loopschedule.h` in particular is meant to be identical — it decides
  where a repeating tile (a birthday, a holiday) goes next, and both programs
  have to agree. **Change one, change the other.**

---

## Versioning

`src/notifierversion.h` is the only place the version is written; CMake reads
it from there. It deliberately tracks the main app's number — they ship
together and share a data file, so two version lines would only raise the
question of which went with which.

---

## Licence

None yet — all rights reserved for now.
