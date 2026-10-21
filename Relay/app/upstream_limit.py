"""
Upstream rate limiting — V5.

rate_limit.py caps what any ONE installation may ask of the relay. This
caps what the relay may ask of TMDB/IGDB, which is a different problem
and the one that actually risks a ban: fifty installations each politely
inside their own per-minute budget still add up to a flood upstream. The
per-client limiter cannot see that, because it only ever looks at one
client at a time.

Published limits this is built around (verified against the providers'
own documentation, not assumed):

  TMDB  ~40-50 requests/second, 20 concurrent connections per IP.
        No daily cap, and no request-count limit on image.tmdb.org at
        all — images are constrained only by concurrency, and the
        desktop app downloads those directly rather than through here.

  IGDB  4 requests/second, maximum 8 open requests at any moment.
        Substantially stricter, and the reason the shared cache matters
        more for games than for films. The old 10k/month free-tier quota
        no longer exists — API v4 replaced it with this rate limit.

Both are configured well under those ceilings. Headroom costs nothing
here: a few hundred milliseconds of extra pacing is invisible next to a
tile refresh, whereas tripping a limit risks the credential every user
depends on.

A 429 from upstream parks ALL traffic to that provider until the
Retry-After it asked for has passed, rather than letting each request
independently discover the same wall.
"""
import asyncio
import time
from contextlib import asynccontextmanager

from app.config import settings


class UpstreamLimiter:
    """Paces outbound requests: a minimum gap between starts, a ceiling on
    how many may be in flight, and a shared cooldown honouring 429s."""

    def __init__(self, name: str, rate_per_second: float, max_concurrent: int):
        self.name = name
        self._min_interval = 1.0 / rate_per_second if rate_per_second > 0 else 0.0
        self._sem = asyncio.Semaphore(max_concurrent)
        self._gate = asyncio.Lock()
        self._next_slot = 0.0
        self._cooldown_until = 0.0
        self.max_concurrent = max_concurrent
        self.rate_per_second = rate_per_second

        # Observability — surfaced on the admin dashboard so throttling is
        # visible rather than mysterious slowness.
        self.throttled_count = 0
        self.rejected_429_count = 0
        self.total_wait_seconds = 0.0

    @asynccontextmanager
    async def slot(self):
        """Hold one upstream slot for the duration of a request."""
        async with self._sem:
            waited = 0.0
            async with self._gate:
                now = time.monotonic()

                # A 429 pauses everyone, not just whoever hit it.
                if now < self._cooldown_until:
                    waited += self._cooldown_until - now
                    await asyncio.sleep(self._cooldown_until - now)
                    now = time.monotonic()

                if now < self._next_slot:
                    waited += self._next_slot - now
                    await asyncio.sleep(self._next_slot - now)
                    now = time.monotonic()

                self._next_slot = now + self._min_interval

            if waited > 0:
                self.throttled_count += 1
                self.total_wait_seconds += waited
            yield

    def note_response(self, status_code: int, retry_after: str | None = None):
        """Feed an upstream status back in so a 429 actually slows us down."""
        if status_code != 429:
            return
        self.rejected_429_count += 1
        delay = 5.0
        if retry_after:
            try:
                delay = max(delay, float(retry_after))
            except (TypeError, ValueError):
                pass
        self._cooldown_until = max(self._cooldown_until, time.monotonic() + delay)

    def stats(self) -> dict:
        return {
            "name": self.name,
            "rate_per_second": self.rate_per_second,
            "max_concurrent": self.max_concurrent,
            "in_flight": self.max_concurrent - self._sem._value,   # noqa: SLF001 - display only
            "throttled": self.throttled_count,
            "upstream_429s": self.rejected_429_count,
            "total_wait_seconds": round(self.total_wait_seconds, 2),
            "cooling_down": time.monotonic() < self._cooldown_until,
        }


tmdb = UpstreamLimiter("tmdb", settings.TMDB_RATE_PER_SECOND, settings.TMDB_MAX_CONCURRENT)
igdb = UpstreamLimiter("igdb", settings.IGDB_RATE_PER_SECOND, settings.IGDB_MAX_CONCURRENT)


def all_stats() -> list[dict]:
    return [tmdb.stats(), igdb.stats()]
