# Media Countdowns

A Windows desktop app that counts down to the things you're waiting for —
episodes, films and games — as a wall of tiles with the artwork on them.

It's free and non-commercial, deliberately: that's the basis on which TMDB and
IGDB allow use of their free tiers.

This repository is the full source. **It ships with no API keys in it**, which
is the one thing you need to know before building: keys committed to a public
repository are keys anyone can take, so there aren't any here. Everything below
is about filling in your own — and about the fact that you may not need to.

---

## The short version

**TV works with no setup at all.** Episode dates and air times come from
[TVmaze](https://www.tvmaze.com/api), which is free, public and needs no key.
Build it and add a show and it works.

**Films and games need credentials**, because TMDB and IGDB both require them.
You have two ways to provide them, and you only need one:

| | What it is | Best when |
|---|---|---|
| **Your own keys** | Paste a TMDB key and IGDB credentials into Settings. The app calls those services directly. | You just want the app working on your own machine. Start here. |
| **Your own relay** | Run the small server from [the relay](../Relay/README.md) (in this repository), which holds the keys and answers on their behalf. | You want several machines, or the household, sharing one set of keys. |

Nothing is hidden behind the relay. The app is fully usable with your own keys
and no server at all.

---

## Building

You need **Qt 6.7.3** (MinGW 64-bit), **CMake 3.21+** and **Ninja**. The Qt
Online Installer gives you all three — pick the MinGW 64-bit toolchain under
your Qt version, and "CMake" and "Ninja" under Developer and Designer Tools.

The easy way is to open `CMakeLists.txt` in **Qt Creator** and press build.

From a command line, with Qt's MinGW and Ninja on your `PATH`:

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.7.3/mingw_64" -DCMAKE_BUILD_TYPE=Release
```

then:

```bash
cmake --build build
```

The result is `build/MediaCountdowns.exe`. To run it outside Qt Creator you
need Qt's DLLs beside it, which `windeployqt` copies in for you:

```bash
C:/Qt/6.7.3/mingw_64/bin/windeployqt.exe build/MediaCountdowns.exe
```

There's a companion tray program, [the
Notifier](../Notifier/README.md),
which is what actually pops up a notification when something releases. The app
works without it; you just won't be told about a release unless the app is
open and you're looking at it.

---

## Setting up your own keys

Everything here is in **Settings → Network**, and takes effect immediately — no
rebuild, and nothing is written into the executable.

### TMDB — films, and the artwork on every tile

1. Make a free account at [themoviedb.org](https://www.themoviedb.org/signup).
2. Go to **Settings → API** and request an API key. Choose the **Developer**
   option. It's free, and approval is usually instant.
3. Copy the **API Key (v3 auth)** — the short one, roughly 32 characters, not
   the very long "Read Access Token".
4. In Media Countdowns: **Settings → Network → TMDB API Key**, paste, Save.

### IGDB — games

IGDB authenticates through Twitch, so this is a Twitch developer app rather
than an IGDB one. It's still free.

1. Sign in at
   [dev.twitch.tv/console/apps](https://dev.twitch.tv/console/apps) and
   **Register Your Application**.
2. Name it anything. OAuth Redirect URL: `http://localhost`. Category: choose
   Application Integration.
3. Copy the **Client ID**, then press **New Secret** and copy the **Client
   Secret** — Twitch shows a secret once and never again.
4. In Media Countdowns: **Settings → Network → IGDB API Credentials**, paste
   both, Save.

### How the app decides what to use

- A service you've given credentials for is always called **directly**. Your
  own key beats everything.
- A service you haven't is asked through the relay, if you've set one up.
- If you've configured **both** TMDB and IGDB yourself, the **Use Media
  Countdowns Server** checkbox becomes a straight switch for both at once —
  tick it to route through your relay anyway, untick it to go direct.
- With no keys and no relay, the app runs on TVmaze alone. TV works; films and
  games have nothing to search with.

---

## Pointing the app at a relay

Only if you're running one. **Settings → Network → Relay Key** takes two
things: the **address**, and the **shared secret** (the `RELAY_SHARED_SECRET`
from the relay's `.env` file).

### The address does not have to be a domain

This is the part people over-think. A domain name is only needed if you want to
reach the relay from *outside* your home. Pick whichever of these describes you:

| Where the relay runs | What to enter | Needs |
|---|---|---|
| **This same machine** | `http://localhost:8080` | nothing |
| **Another machine on your network** | `http://192.168.1.20:8080` — that machine's local address | port 8080 through its firewall |
| **Reachable from anywhere** | `https://your-name.duckdns.org` | a domain, HTTPS, and a forwarded port |

The first two are the common cases and the ones to start with. Nothing is
exposed to the internet, there's no certificate to manage, no router settings,
and no third-party service involved. If it's just you on one PC, use
`localhost` and you're done.

Only reach for the third row if you genuinely need the app to work away from
home — and understand what it means: a port open to the world, a server you're
responsible for keeping patched, and a shared secret that is the only thing
standing between the internet and your API keys. The relay's README walks
through DuckDNS and Caddy if you decide you want it.

To find a machine's local address, run `ipconfig` on it (Windows) or
`ip addr` / `ifconfig` (Linux, macOS) and look for the `192.168.x.x` or
`10.x.x.x` one. A router setting called "DHCP reservation" will stop it
changing on you.

### Until you fill both in

Both fields are blank in a fresh build, and until **both** are set the app
doesn't call a relay at all — it doesn't try and fail, it simply doesn't try,
so you won't see errors for a server you never set up. Settings will say
"Not set up — add a Relay Key below", which is a different thing from the
server being down.

---

## Where your data lives

- `%APPDATA%\MediaCountdowns\tiles.json` — your tiles
- `%APPDATA%\MediaCountdowns\release_history.json` — the Recap/History record
- `%APPDATA%\MediaCountdowns\fetched_images\`, `custom_images\` — artwork
- `HKEY_CURRENT_USER\Software\HijackAssassin\MediaCountdowns` — settings

**Settings → Backup → Export** writes all of that to a single zip, images
included, and Import reads one back. That's the supported way to move to
another machine.

---

## Data sources and attribution

- **TVmaze** — episode dates and air times. Data is CC BY-SA; credited on the
  app's About page.
- **TMDB** — film and show search, credits and backdrops. This product uses the
  TMDB API but is not endorsed or certified by TMDB.
- **IGDB** — game search, details and artwork.

Please read each service's terms before running this against your own keys.
TMDB's forbid caching their data beyond six months, which is why the app
re-fetches artwork on a 150-day cycle rather than keeping it forever.

---

## Licence

None yet — all rights reserved for now. You're welcome to build it and run it
for yourself.
