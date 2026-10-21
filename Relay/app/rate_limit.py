"""
A basic fixed-window rate limiter, per client installation.

Since everyone using this relay shares the same underlying TMDB/IGDB
quota, one runaway or misbehaving client (a bug in someone's build, or
someone hammering the search box) could exhaust it for everyone. This
caps how many requests any single installation can make per minute —
cheap, in-memory, good enough for a personal/small-group relay. Keyed by
the per-installation client ID (see auth.py) rather than the shared
secret, so this is a fair, per-person budget, not one pool everyone
draws from together.
"""
import time
from collections import defaultdict, deque
from fastapi import HTTPException
from app.config import settings

_windows: dict[str, deque] = defaultdict(deque)

# ── /register, which is the one route that cannot be limited per installation ─
#
# Every other endpoint is keyed by the caller's client id, which is fair and
# equal: 300 a minute each. /register is where an id comes FROM, so the caller
# has none yet and there is nothing to key on — which made it the way around
# every other limit. Anyone holding the shared secret could mint ids in a loop
# and hand each one its own full budget, so a hundred ids meant a hundred times
# the traffic, and the installation count filled with machines that never
# existed.
#
# The only thing available to key on is where the request came from, and a real
# installation registers exactly once in its life. A handful per hour is
# therefore enormously generous for genuine use — several reinstalls, or a
# household behind one address — while a minting loop hits the wall almost at
# once.
_register_windows: dict[str, deque] = defaultdict(deque)

REGISTER_MAX_PER_HOUR = 10
REGISTER_WINDOW_SECONDS = 3600


def client_address(request) -> str:
    """
    The caller's address, as well as it can be known.

    Caddy sits in front of uvicorn, so request.client.host is Caddy itself
    (127.0.0.1) for every request in production — using it directly would put
    the whole internet in one bucket. Caddy APPENDS the peer it actually saw to
    X-Forwarded-For, so the LAST entry is the one it observed and the only one
    a client cannot forge; earlier entries may be anything the caller sent.
    """
    forwarded = request.headers.get("x-forwarded-for", "")
    if forwarded:
        return forwarded.split(",")[-1].strip()
    return request.client.host if request.client else "unknown"


def check_register(address: str):
    now = time.monotonic()
    window = _register_windows[address]
    while window and now - window[0] > REGISTER_WINDOW_SECONDS:
        window.popleft()
    if len(window) >= REGISTER_MAX_PER_HOUR:
        raise HTTPException(status_code=429,
                            detail="Too many registrations — try again later")
    window.append(now)


def check(client_key: str):
    now = time.monotonic()
    window = _windows[client_key]
    while window and now - window[0] > 60:
        window.popleft()
    if len(window) >= settings.RATE_LIMIT_PER_MINUTE:
        raise HTTPException(status_code=429, detail="Rate limit exceeded — slow down")
    window.append(now)
