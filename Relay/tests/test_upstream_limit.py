"""
Verifies the upstream limiters actually hold the published ceilings under
a burst, since the whole point is protecting a shared credential that
every installation depends on.
"""
import asyncio
import time

import pytest

from app.upstream_limit import UpstreamLimiter


async def _hammer(limiter: UpstreamLimiter, count: int, work_seconds: float = 0.0):
    """Fire `count` requests at once; record start times and peak concurrency."""
    starts: list[float] = []
    concurrent = 0
    peak = 0
    lock = asyncio.Lock()

    async def one():
        nonlocal concurrent, peak
        async with limiter.slot():
            async with lock:
                concurrent += 1
                peak = max(peak, concurrent)
            starts.append(time.monotonic())
            if work_seconds:
                await asyncio.sleep(work_seconds)
            async with lock:
                concurrent -= 1

    await asyncio.gather(*(one() for _ in range(count)))
    return sorted(starts), peak


@pytest.mark.asyncio
async def test_igdb_never_exceeds_four_per_second():
    """IGDB's documented limit is 4 req/s; we pace at 3 to leave headroom."""
    limiter = UpstreamLimiter("igdb-test", rate_per_second=3, max_concurrent=6)
    starts, _ = await _hammer(limiter, 12)

    # No 1-second window may contain more than 4 request starts.
    for i, t0 in enumerate(starts):
        in_window = sum(1 for t in starts[i:] if t - t0 < 1.0)
        assert in_window <= 4, f"{in_window} requests started within one second"


@pytest.mark.asyncio
async def test_igdb_never_exceeds_eight_open_requests():
    """IGDB allows at most 8 in flight; we cap at 6."""
    limiter = UpstreamLimiter("igdb-test", rate_per_second=50, max_concurrent=6)
    _, peak = await _hammer(limiter, 30, work_seconds=0.15)
    assert peak <= 6, f"peak concurrency was {peak}, above the configured cap"
    assert peak <= 8, "exceeded IGDB's documented 8 open requests"


@pytest.mark.asyncio
async def test_tmdb_stays_under_twenty_concurrent():
    """TMDB allows 20 connections per IP; we cap at 10."""
    limiter = UpstreamLimiter("tmdb-test", rate_per_second=20, max_concurrent=10)
    _, peak = await _hammer(limiter, 60, work_seconds=0.05)
    assert peak <= 10
    assert peak <= 20, "exceeded TMDB's documented 20 concurrent connections"


@pytest.mark.asyncio
async def test_429_pauses_all_traffic_not_just_the_caller():
    """A 429 must park every request to that provider, not only the one
    that happened to receive it."""
    limiter = UpstreamLimiter("test", rate_per_second=100, max_concurrent=5)
    limiter.note_response(429, retry_after="1")

    t0 = time.monotonic()
    async with limiter.slot():
        pass
    waited = time.monotonic() - t0

    assert waited >= 0.9, f"only waited {waited:.2f}s after a 429"
    assert limiter.rejected_429_count == 1
    assert limiter.stats()["upstream_429s"] == 1


@pytest.mark.asyncio
async def test_normal_responses_do_not_trigger_cooldown():
    limiter = UpstreamLimiter("test", rate_per_second=100, max_concurrent=5)
    limiter.note_response(200)
    limiter.note_response(404)
    assert limiter.stats()["cooling_down"] is False
    assert limiter.rejected_429_count == 0


# ── The configuration actually shipped must stay inside the providers'
#    published ceilings. These guard against someone (including a future
#    me) tuning for speed and quietly crossing a real limit.
#
#    Published, verified against provider documentation:
#      TMDB  ~40 req/s, 20 concurrent connections per IP
#      IGDB  4 req/s, 8 open requests maximum
TMDB_PUBLISHED_RATE, TMDB_PUBLISHED_CONCURRENT = 40, 20
IGDB_PUBLISHED_RATE, IGDB_PUBLISHED_CONCURRENT = 4, 8


def test_shipped_tmdb_config_is_within_published_limits():
    from app.config import settings
    assert settings.TMDB_RATE_PER_SECOND <= TMDB_PUBLISHED_RATE * 0.85, (
        "TMDB rate is too close to the published ceiling — leave headroom for "
        "bursts from multiple installations"
    )
    assert settings.TMDB_MAX_CONCURRENT <= TMDB_PUBLISHED_CONCURRENT * 0.85


def test_shipped_igdb_config_is_within_published_limits():
    from app.config import settings
    assert settings.IGDB_RATE_PER_SECOND <= IGDB_PUBLISHED_RATE * 0.85, (
        "IGDB rate is too close to its 4/s ceiling — a 429 here affects every "
        "user at once, since one credential serves everyone"
    )
    assert settings.IGDB_MAX_CONCURRENT <= IGDB_PUBLISHED_CONCURRENT * 0.85


def test_cache_ttl_never_exceeds_tmdb_six_month_rule():
    """TMDB's terms forbid caching anything beyond 6 months."""
    from app.config import settings
    six_months = 180 * 24 * 3600
    assert settings.CACHE_TTL_MAX_SECONDS <= six_months
    for name in ("CACHE_TTL_SEARCH_SECONDS", "CACHE_TTL_DETAILS_SECONDS",
                 "CACHE_TTL_GAME_META_SECONDS"):
        assert getattr(settings, name) <= six_months, f"{name} exceeds 6 months"


def test_release_date_ttl_stays_short():
    """A release date can genuinely move. Showing a stale one is the worst
    failure this app has, so this TTL must stay short regardless of what
    the compliance ceiling happens to allow."""
    from app.config import settings
    assert settings.CACHE_TTL_DETAILS_SECONDS <= 12 * 3600, (
        "Release-date cache is too long — dates change and users would see "
        "an outdated countdown"
    )
