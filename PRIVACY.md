# Privacy Policy — Media Countdowns

**Last updated: 15 August 2026**

Media Countdowns is a desktop app that counts down to films, TV episodes and
games. This page describes exactly what it sends, where it goes, and how long
it is kept.

There is no account, no sign-in, and no advertising. The app never asks for
your name, your email address, or your location, and it has no way to contact
you.

---

## What stays on your computer

Everything you create in the app. Your tiles, the dates and images on them,
your settings and your release history are stored only on your own PC:

```
%APPDATA%\MediaCountdowns\                      tiles, history, images
HKEY_CURRENT_USER\Software\HijackAssassin\MediaCountdowns    settings
```

None of it is uploaded anywhere. Uninstalling the app leaves these files in
place; delete the folder yourself if you want them gone.

---

## What leaves your computer

### Looking up films, shows and games

When you search for a title, or when the app refreshes a tile, it asks a data
source for that title's details. The request contains the **title you searched
for or the tile you are refreshing** — nothing about you.

Depending on how the app is set up, that request goes either directly from your
PC to the data source, or through the Media Countdowns server, which passes it
on. The sources are:

- **TVmaze** — TV episode dates and air times ([privacy policy](https://www.tvmaze.com/privacy))
- **TMDB** — film and show details and artwork ([privacy policy](https://www.themoviedb.org/privacy-policy))
- **IGDB / Twitch** — game details and artwork ([privacy policy](https://www.twitch.tv/p/legal/privacy-notice/))

As with any request over the internet, these services can see your IP address.
That is inherent to how the internet works and is not something the app adds.

### The anonymous installation ID

When the app uses the Media Countdowns server, it sends a randomly generated
**installation ID** — a string of random characters, created on your PC the
first time it runs. It exists so the server can count how many installations
there are and apply fair usage limits per installation rather than punishing
everyone for one heavy user.

It is not derived from your name, your hardware, your Windows account or
anything else about you, and it cannot be traced back to you. It is stored on
your PC in `%USERPROFILE%\.mediacountdowns\install.id` and in the registry key
above. Deleting both gives you a new one.

### Corrections you choose to report

If you press the "report this air time" action, or when the app notices that a
real release date has replaced an announced release window, it sends the
**show's title and identifier** and the anonymous installation ID. This is how
a wrong air time gets fixed for everyone. No other information is included.

### Feedback you choose to send

If you use **Settings → Feedback** to report a problem or suggest a feature,
the app sends **the text you typed** and **the app version**, along with the
anonymous installation ID. Nothing else is attached — there is no name, no
email address, and no way to reply to you.

Because the message is free text, please do not type anything into it that you
would not want stored, such as your email address or other personal details.

---

## The Media Countdowns server

The server is run by the app's developer. It exists so the app can look things
up without every user needing their own API keys.

It records, for each request: the anonymous installation ID, which kind of
request it was, and the time. It caches the answers it gets from TMDB and IGDB
so that repeated lookups do not have to be made again — TMDB's terms require
that cached data be discarded within six months, and it is.

Feedback messages and air-time reports are kept until they have been read and
dealt with, then deleted.

The server does not build a profile of you, does not sell or share anything
with third parties, and has nothing to share that could identify you.

---

## Running it without the server

You do not have to use the Media Countdowns server at all:

- Turn off **Settings → Network → Use Media Countdowns Server** and the app
  stops contacting it entirely.
- Enter your own TMDB key and IGDB credentials in Settings, and lookups go
  directly from your PC to those services.
- Run your own copy of the server — it is open source — and point the app at
  it under **Settings → Network → Relay Key**.

TV episode dates come from TVmaze, which needs no account or key.

---

## Children

The app is not directed at children and does not knowingly collect information
from anyone. It has no way to know who is using it.

---

## Changes

If this policy changes, the "last updated" date above changes with it, and the
previous versions remain visible in this repository's history.

## Contact

Questions about this policy: **Patrickjp292004@gmail.com**
