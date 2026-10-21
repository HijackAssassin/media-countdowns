# Media Countdowns

A Windows app that counts down to the things you're waiting for - TV episodes, films and games - as a wall of
tiles with the artwork on them. Search for something, add it, and its tile counts down to the release; a small
tray program (the Notifier) pops up a notification when it comes out, even when the app is closed.

**Download:** https://teamwolfgamers.online/media-countdowns

The app checks that page for updates each time it starts (pattywatty.duckdns.org is the backup address).

## What's in here

| Folder | What it is |
|---|---|
| `Manager\` | The app itself (Qt 6 / C++). Its [README](Manager/README.md) explains the keys and the relay; [PRIVACY.md](Manager/PRIVACY.md) is the privacy policy. |
| `Notifier\` | The tray program that sends the release notifications ([README](Notifier/README.md)). |
| `Relay\` | The optional small server (Python, FastAPI) that holds the TMDB/IGDB keys so installed apps don't need their own ([README](Relay/README.md)). |
| `Media Countdowns.iss` | The Inno Setup installer script (`package-app.bat` runs it). |

**No keys are in this source.** TV works with no setup at all (TVmaze needs no key); for films and games, put
your own TMDB key / IGDB credentials in Settings, or run your own relay - see `Manager\README.md`.

## Building

Windows. Needs **Qt 6.7.3 for MinGW 64-bit** with the MinGW, CMake and Ninja that come with it (in `C:\Qt`), and
**Inno Setup 6** for the installer. The relay needs Python 3.11+ (`pip install -r Relay\requirements.txt`) and,
only if it is exposed to the internet, [Caddy](https://caddyserver.com/download) (`caddy.exe`, not included).

This folder is the app's source. In Patrick's project it sits inside the project folder, whose `build.bat` runs
`build-app.bat <version> <project folder>` and leaves the app in `Build\` (`MediaCountdowns.exe`, and the
notifier in `Build\Notifier\`); `package-app.bat` makes the installer `Media-Countdowns-V<version>-Setup.exe`.
Each program can also be built on its own from its folder with CMake or Qt Creator - see its README.

## More

Made by Patrick - more apps and tools at https://teamwolfgamers.online
