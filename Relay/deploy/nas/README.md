# Moving the relay to the My Cloud EX2 Ultra

Everything needed to run the relay on the NAS in Docker, and to retire the
Windows machine as the host.

**Read the "Two things that will break it" section before you start.** Both are
easy to avoid and unpleasant to discover afterwards.

---

## What the NAS is, and why it matters

The EX2 Ultra is a Marvell Armada 385: **ARMv7, dual-core 1.3 GHz, 1 GB RAM**.
That is a small machine, and it drives two decisions:

- **The image is built on the NAS**, so it is automatically the right
  architecture. No cross-building, nothing to configure.
- **Nothing may need compiling.** `requirements-nas.txt` is the root
  `requirements.txt` minus the `uvicorn[standard]` extra, because that extra
  pulls in `uvloop`, `httptools`, `watchfiles` and `websockets` — the packages
  least likely to have an ARMv7 wheel, and therefore the ones pip would try to
  build from source on a machine that cannot finish the job. None of them are
  used: the dashboard streams with Server-Sent Events, not WebSockets, and
  `--reload` is a development thing.

The relay itself is a light load — a few requests a minute — so 1 GB is
comfortable once it is running. It is only the *build* that is tight.

---

## Two things that will break it

### 1. You must bring `relay.db` with you

It is not just a cache. It holds the **client-id signing secret**, generated
once and kept in the `relay_config` table. Every installed copy of the app has
an id signed with it.

Start with an empty database and every one of those ids fails verification, so
every installation looks new, re-registers, and the installation history you
have been building resets. Copy the file across.

### 2. `RELAY_SHARED_SECRET` must not change

It is what every installed app sends to authenticate. Change it and they are
all locked out at once, with no way to tell them the new one — the value is
baked into builds already in people's hands.

Copy `.env` over unchanged. Do not "tidy up" or regenerate anything in it.

---

## Step 1 — Give the NAS a fixed address

In your router's DHCP settings, reserve an address for the NAS by its MAC
address. Port forwarding points at an IP; if the NAS's changes on a reboot,
everything below silently stops working.

Write it down — this guide calls it `NAS_IP`.

---

## Step 2 — Copy the relay onto the NAS

Copy the whole `C:\Media Countdowns Relay Server` folder to a share on the NAS,
for example `/mnt/HD/HD_a2/docker/mc-relay`. It must include:

- `app/` — the code
- `deploy/nas/` — this folder
- `.env` — your real secrets

Then add three values to `.env`, which the compose file needs and the Windows
setup never did:

```
RELAY_DOMAIN=yourname.duckdns.org
DUCKDNS_DOMAIN=yourname
DUCKDNS_TOKEN=<the token from duckdns.org>
```

`DUCKDNS_DOMAIN` is the subdomain only — `yourname`, not the full hostname.

> If your router has its own DuckDNS client, use that and delete the `duckdns`
> service from `docker-compose.yml`. Two updaters is pointless, though harmless.

---

## Step 3 — Stop the Windows relay first

Before the NAS goes live, on the PC:

1. Run **`Disable Relay Autostart.bat`**, so it does not come back on reboot.
2. Close both `MC Relay` windows (uvicorn and caddy).

This matters more than it looks. Two Caddy instances answering for the same
hostname will both try to renew the certificate, and Let's Encrypt rate-limits
failures — **five per hour, fifty certificates per week per domain**. Getting
that wrong locks you out of issuing certificates for the rest of the week.

---

## Step 4 — Deploy the stack in Portainer

**Stacks → Add stack → Repository**, or **Web editor** and paste
`docker-compose.yml`. If pasting, set the working directory so the relative
paths (`../..` for the build context, `./Caddyfile`) resolve — deploying from
the copied folder is simpler than fighting that.

Then **Deploy the stack**.

The first build downloads Python and installs the dependencies. On this
hardware **expect 10–20 minutes**, and watch the log rather than assuming it
has hung.

**If the build fails on `pydantic-core`**, that is the Rust package that has no
ARMv7 wheel. Uncomment the last line of `requirements-nas.txt` to pin
pydantic 1.x — pure Python, no compiler — and redeploy. The relay never imports
pydantic itself, so nothing about its behaviour changes.

---

## Step 5 — Restore the database

The stack creates an empty `relay-data` volume. Put your real database in it:

```sh
docker cp "/mnt/HD/HD_a2/docker/mc-relay/relay.db" mc-relay:/data/relay.db
docker restart mc-relay
```

Portainer can do this too — **Containers → mc-relay → Console**, or its volume
browser if the version you have has one.

Check it took: the admin dashboard should show your existing installation count
and history, not zeroes.

---

## Step 6 — Move the port forwards

This is the part you asked about, and it is a straight swap. In the router:

| | Before | After |
|---|---|---|
| Internet **:80** | → PC, port 80 | → **NAS_IP, port 8080** |
| Internet **:443** | → PC, port 443 | → **NAS_IP, port 8443** |

**Delete the two rules pointing at the PC.** Leaving them creates a conflict —
the router will not send the same external port to two machines, and which one
wins is not something you want to be guessing about.

### Why the NAS-side ports are different

The NAS already serves its own admin interface on 80 and 443, so Caddy cannot
have them. It is published on 8080 and 8443 instead, and the router translates.
Inside its container Caddy still listens on the normal 80 and 443, so
certificate issuance works exactly as before — Let's Encrypt connects to your
public address on 80 and 443, and the router does the rest.

Keep **port 80 forwarded**. It is not serving traffic, but Caddy uses it for
the HTTP-01 challenge when renewing, and renewal failing quietly three months
from now is a bad way to find out.

---

## Step 7 — Check it

From another machine (not the LAN, if you can — phone on mobile data is ideal,
since some routers do not loop traffic back to themselves):

```
https://yourname.duckdns.org/health     -> {"status":"ok"}
https://yourname.duckdns.org/admin      -> the dashboard login
```

Then open Media Countdowns and press refresh. It needs no reconfiguration —
same hostname, same secret, so it cannot tell the difference. That is the whole
goal.

Finally, check the dashboard shows your real numbers. If it shows zeroes,
step 5 did not take.

---

## Afterwards

- **Back up the `caddy-data` volume**, or at least know it exists. It holds the
  certificates; losing it means re-issuing, and that is rate-limited.
- **`relay-data` holds the database** — the installation history, the
  corrections, the feedback. Worth including in whatever the NAS already backs
  up.
- Updating the relay: change the code, then **Stacks → mc-relay → Update the
  stack** with *re-pull and redeploy*. The volumes survive; only the image is
  rebuilt.

---

## What has and has not been verified

Written against this repository's actual code — `RELAY_DB_PATH` is a real
environment variable that `app/db.py` reads, the dependency list is this
project's own, and the `X-Forwarded-For` note matches how `app/rate_limit.py`
behaves.

**None of it has been run.** There is no Docker on the Windows machine to build
against, and no way to test an ARMv7 image from here. The likely failure point
is the dependency install in step 4 — which is why the pydantic fallback is
written down rather than left to be worked out on the night.
