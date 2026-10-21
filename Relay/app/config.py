"""
Configuration for the Media Countdowns Relay server.

Everything here is loaded from environment variables (or a local .env
file during development — see .env.example). Nothing secret is ever
hardcoded, and none of it ships inside the Media Countdowns desktop app —
that's the whole point of this server existing.
"""
import os
from dotenv import load_dotenv

load_dotenv()


def _require(name: str, default: str | None = None) -> str:
    val = os.environ.get(name, default)
    if val is None:
        raise RuntimeError(
            f"Missing required environment variable: {name}. "
            f"Copy .env.example to .env and fill it in."
        )
    return val


class Settings:
    # --- TMDB ---
    TMDB_API_KEY: str = os.environ.get("TMDB_API_KEY", "")
    TMDB_BASE_URL: str = "https://api.themoviedb.org/3"

    # --- IGDB (via Twitch OAuth2, client-credentials flow) ---
    IGDB_CLIENT_ID: str = os.environ.get("IGDB_CLIENT_ID", "")
    IGDB_CLIENT_SECRET: str = os.environ.get("IGDB_CLIENT_SECRET", "")
    IGDB_BASE_URL: str = "https://api.igdb.com/v4"
    TWITCH_TOKEN_URL: str = "https://id.twitch.tv/oauth2/token"

    # --- Relay's own auth: a shared secret the desktop app must send. ---
    # This is NOT the same as the TMDB/IGDB credentials — it's what
    # gates access to *this* relay so it isn't a wide-open proxy anyone
    # on the internet can hammer using your API quota.
    RELAY_SHARED_SECRET: str = os.environ.get("RELAY_SHARED_SECRET", "")

    # --- Admin dashboard login (separate from the client shared secret
    # above — this gates the usage stats/MOTD page, not the API itself) ---
    ADMIN_PASSWORD: str = os.environ.get("ADMIN_PASSWORD", "")
    SESSION_SECRET: str = os.environ.get("SESSION_SECRET", os.environ.get("ADMIN_PASSWORD", "insecure-dev-secret"))

    # --- Operational knobs ---
    # v2 — search results (what titles match a query) rarely change, so
    # these can be cached longer; a detail/date fetch (release dates,
    # episode air dates) is exactly the kind of thing that CAN shift, so
    # that gets a shorter TTL to stay fresher. See describe.py's
    # is_search_request() for how a request is classified as one or the
    # other.
    CACHE_TTL_SEARCH_SECONDS: int = int(os.environ.get("CACHE_TTL_SEARCH_SECONDS", str(24 * 3600)))
    CACHE_TTL_DETAILS_SECONDS: int = int(os.environ.get("CACHE_TTL_DETAILS_SECONDS", str(6 * 3600)))

    # V5 — TMDB's terms of use forbid caching "for longer than 6 months" any
    # information obtained from the API. Every TTL is clamped to this in
    # cache.set(), so no future tuning of the values above (or an env var set
    # by hand) can drift out of compliance by accident.
    CACHE_TTL_MAX_SECONDS: int = 180 * 24 * 3600   # 6 months

    # V5 — game metadata (title, release year, developer, publisher) barely
    # changes once a title is published, and IGDB is by far the stricter of
    # the two providers, so this is worth holding longer than a film's
    # release date. Deliberately NOT applied to dates: a release date really
    # can move, and showing a stale one is the single worst failure this app
    # has, so those stay on the short details TTL above.
    CACHE_TTL_GAME_META_SECONDS: int = int(os.environ.get("CACHE_TTL_GAME_META_SECONDS", str(7 * 24 * 3600)))

    # --- Upstream pacing (see upstream_limit.py) ---
    #
    # The two providers are limited by genuinely different things, so they
    # are tuned differently rather than given one shared "safe" number:
    #
    #   TMDB — ~40-50 requests/second and 20 concurrent connections per IP.
    #     Generous, and the relay is nowhere near it in normal use, so this
    #     runs at 30/s and 15 concurrent: roughly 25-30% under both ceilings.
    #     Fast enough that a full refresh never feels paced, with enough
    #     headroom that a burst from several installations at once still
    #     can't cross the line.
    #
    #   IGDB — 4 requests/second, maximum 8 open. Far stricter, and the
    #     binding constraint is the RATE, not concurrency: raising the
    #     concurrent cap alone would change nothing, because requests still
    #     have to start 1/4s apart. So this stays at 3/s (25% under) and 6
    #     concurrent. Games are also the rarest lookup and the most heavily
    #     cached, so this costs almost nothing in practice — whereas a 429
    #     here is shared by every user at once, since one credential serves
    #     everyone.
    #
    # Raising either past the documented ceiling will fail the tests in
    # tests/test_upstream_limit.py, which assert against the PUBLISHED
    # limits rather than against these values.
    TMDB_RATE_PER_SECOND: float = float(os.environ.get("TMDB_RATE_PER_SECOND", "30"))
    TMDB_MAX_CONCURRENT: int = int(os.environ.get("TMDB_MAX_CONCURRENT", "15"))
    IGDB_RATE_PER_SECOND: float = float(os.environ.get("IGDB_RATE_PER_SECOND", "3"))
    IGDB_MAX_CONCURRENT: int = int(os.environ.get("IGDB_MAX_CONCURRENT", "6"))
    CACHE_MAX_ENTRIES: int = int(os.environ.get("CACHE_MAX_ENTRIES", "5000"))
    # v2 — raised from 60. A single startup refresh alone can be 30-40+
    # requests, and active searching (searches fire in parallel with
    # credits lookups, curated collections fire up to 4 requests at
    # once) easily pushes past 60/minute during completely normal,
    # legitimate use — this was throttling real usage, not runaway bugs.
    RATE_LIMIT_PER_MINUTE: int = int(os.environ.get("RATE_LIMIT_PER_MINUTE", "300"))
    MOCK_UPSTREAM: bool = os.environ.get("MOCK_UPSTREAM", "false").lower() == "true"


settings = Settings()
