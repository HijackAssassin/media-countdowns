# Media Countdowns Relay

A small FastAPI server that sits between [Media
Countdowns](../Manager/README.md) and the
two APIs that need credentials — TMDB and IGDB. It holds the keys, so the app
doesn't have to.

**You do not need this to use the app.** The app works with your own TMDB and
IGDB keys entered straight into its Settings, and TV works with no keys at all.
Run the relay when you'd rather one set of keys served several machines than
paste them into each one.

What it adds, beyond hiding the keys:

- **Rate limiting that actually protects the upstreams** — deliberately below
  TMDB's and IGDB's published ceilings, with a shared cooldown when either
  answers 429.
- **Caching**, so ten machines asking the same question cost one call.
- **Corrections** — a small table of "this show really airs at 9, not
  midnight" that every connected app picks up.
- **An admin dashboard** at `/admin`: usage, a live console, corrections, and
  the bug reports and feature requests the app can send.

---

## Requirements

Python **3.11+**. No database to install — it uses SQLite in a file beside the
app.

```bash
pip install -r requirements.txt
```

---

## Setting it up

### 1. Your credentials

Copy `.env.example` to `.env` and fill it in. **`.env` is git-ignored and must
stay that way** — it holds every secret this server has.

```
TMDB_API_KEY=...
IGDB_CLIENT_ID=...
IGDB_CLIENT_SECRET=...
```

Where those come from is covered in the app's README — same keys, same free
signups ([themoviedb.org](https://www.themoviedb.org/signup) for TMDB,
[dev.twitch.tv](https://dev.twitch.tv/console/apps) for IGDB).

### 2. Your two secrets — generate them, don't invent them

These are the ones people get wrong, so read this bit.

```
RELAY_SHARED_SECRET=...
ADMIN_PASSWORD=...
```

- **`RELAY_SHARED_SECRET`** is what an app installation sends to prove it's
  allowed to use your relay. It goes in the app under **Settings → Network →
  Relay Key**.
- **`ADMIN_PASSWORD`** is what logs *you* into the dashboard at `/admin`.

**They must be different values.** One lets any app in; the other lets someone
change your server's settings. Sharing a value between them means handing the
dashboard to everyone you ever gave the app secret to.

Generate both, rather than typing something memorable. In PowerShell:

```powershell
-join ((48..57)+(65..90)+(97..122) | Get-Random -Count 40 | % {[char]$_})
```

Run it twice and use each answer once. On Linux or macOS, `openssl rand -base64 32`
does the same job.

Neither is recoverable if you lose it — they exist only in your `.env`. Losing
`RELAY_SHARED_SECRET` means re-entering a new one in every app install; losing
`ADMIN_PASSWORD` just means editing `.env` and restarting.

### 3. Run it

```bash
uvicorn app.main:app --host 0.0.0.0 --port 8080
```

Check it's alive at `http://localhost:8080/health` — you should get
`{"status":"ok"}`. The dashboard is at `http://localhost:8080/admin`.

That's a working relay. `--host 0.0.0.0` means it already answers on your local
network too, so other machines in the house can reach it without anything
further; `--host 127.0.0.1` instead if you'd rather it only ever answered on
this one.

On Windows, `start_relay.bat` starts this (and Caddy, if you're using it) in
minimised windows. Edit the path at the top first.

### 4. Point the app at it

In Media Countdowns: **Settings → Network → Relay Key**, then enter the
address and the `RELAY_SHARED_SECRET`. Both are blank in a fresh build, and
until both are filled in the app doesn't call a relay at all.

**The address does not have to be a domain** — see the next section, which is
the one to read before you go looking for one.

---

## Do I need a domain? Almost certainly not

Three ways to reach the relay. Most people want the first, some want the
second, and the third is the only one that needs a domain, a certificate or
anything opened on your router.

### 1. Same machine — `http://localhost:8080`

The relay and the app on one PC. Nothing leaves the computer: no hostname, no
router settings, no certificate, no third-party service. **If it's just you,
stop here** — this is the whole setup.

### 2. Your own network — `http://192.168.1.20:8080`

The relay on one machine, the app on others in the house. Use that machine's
local address, which `ipconfig` (Windows) or `ip addr` (Linux, macOS) will tell
you — the `192.168.x.x` or `10.x.x.x` one.

Two things to do: allow port **8080** through that machine's firewall, and give
it a **DHCP reservation** in your router so the address doesn't change and
quietly break the other machines. Still nothing exposed outside your house.

Plain `http` is fine here. The traffic doesn't leave your network, and the only
thing on it worth protecting is the shared secret.

### 3. From anywhere — `https://your-name.duckdns.org`

Only if you genuinely need the app to work away from home. This is the one with
sharp edges, and it's worth being deliberate: a port open to the world, a
server you are responsible for keeping patched, and a shared secret that is the
only thing between the internet and your API keys. The next section covers it.

---

## Optional: a public hostname, with DuckDNS and Caddy

**Only needed for case 3 above.** Skip this entirely for localhost or LAN.

Most home connections get an address that changes. **DuckDNS** is a free
service that gives you a fixed name pointing at wherever you are now.

1. Sign in at [duckdns.org](https://www.duckdns.org) with any of the logins it
   offers, and pick a subdomain — you get `yourname.duckdns.org`.
2. Set up its updater so the name follows your address when it changes. The
   site's **install** page has a ready-made script for Windows, Linux, macOS
   and most routers.
3. **Forward port 443** on your router to the machine running Caddy. This is
   the step that actually exposes it, and the one to be deliberate about.

Then **Caddy** ([caddyserver.com](https://caddyserver.com/download)) puts HTTPS
in front of the relay, fetching and renewing a real certificate on its own.
Copy `Caddyfile.example` to `Caddyfile`, put your hostname on the first line,
and run `caddy run` beside the relay.

Caddy is not committed here — it's a ~50 MB binary and you should download it
from its own site.

With that in place the app's relay address is
`https://yourname.duckdns.org`, and traffic to it is encrypted.

If any of that turns out to be more trouble than it's worth, cases 1 and 2
above are still there and need none of it.

---

## Tests

```bash
pip install -r requirements-dev.txt
python -m pytest tests -q
```

110 tests, covering auth, rate limiting, caching, the corrections table and the
admin routes. They use a scratch SQLite file and never touch a live database.

There's also `MOCK_UPSTREAM=true` in `.env`, which answers with canned data
instead of calling TMDB or IGDB — useful for looking at the dashboard before
you have real keys.

**Never point a real app install at a relay running `MOCK_UPSTREAM=true`.** The
app saves what it's told, and stub payloads will overwrite real tile data with
placeholders.

---

## Licence

None yet — all rights reserved for now.
