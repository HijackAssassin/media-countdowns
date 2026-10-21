"""
Response cache — thin wrapper around app.db's SQLite-backed storage.
Kept as its own module (rather than calling app.db directly from
main.py) so the caching *policy* (what counts as a cache key, when to
bump which stat) stays separate from the raw storage mechanics.
"""
import hashlib
from app import db
from app.config import settings


def make_key(method: str, path: str, body: bytes | None) -> str:
    h = hashlib.sha256()
    h.update(method.encode())
    h.update(path.encode())
    if body:
        h.update(body)
    return h.hexdigest()


def get(key: str):
    result = db.cache_get(key)
    if result is not None:
        db.bump_stat("cache_hits")
    else:
        db.bump_stat("cache_misses")
    return result


def set(key: str, method: str, path: str, body: bytes, status: int, ttl_seconds: int):
    # V5 — hard compliance ceiling. TMDB's API terms of use state plainly
    # that you may not "Cache, for longer than 6 months, any information
    # obtained through or from TMDB or the TMDB APIs." Clamping here rather
    # than at each call site means every cached response passes through this
    # one check, so no future TTL tuning — or an env var set by hand — can
    # quietly put the relay out of compliance.
    #
    # Note this is a CEILING, not a target. The TTLs actually in use are far
    # shorter on purpose: a release date can genuinely move, and showing a
    # stale one is the worst thing this app can do, so dates stay on a
    # 6-hour TTL. Only near-immutable data goes anywhere near this bound.
    ttl_seconds = min(ttl_seconds, settings.CACHE_TTL_MAX_SECONDS)
    db.cache_set(key, method, path, body, status, ttl_seconds)
    db.bump_stat("unique_fetches_ever")


def stats() -> dict:
    return db.dashboard_stats()
