"""
TMDB proxy.

Much simpler than IGDB's — TMDB just wants an api_key query parameter on
every request, no OAuth dance. The relay injects the real key here so it
never has to live in the desktop app or travel over the wire from the
client at all.
"""
import httpx
from fastapi import HTTPException
from app.config import settings
from app import upstream_limit

# v4 — removed search/collection: the client no longer does fuzzy
# collection auto-detection at all (it once matched "avengers" against
# the wrong, unrelated "Avengers Grimm Collection") — replaced entirely
# by an exact, hand-picked list of collection IDs fetched directly via
# collection/{id}.
ALLOWED_TMDB_PREFIXES = ("search/multi", "movie/", "tv/", "collection/")


async def forward(path: str, query: dict, client: httpx.AsyncClient) -> httpx.Response:
    if not any(path.startswith(p) for p in ALLOWED_TMDB_PREFIXES):
        raise HTTPException(status_code=404, detail=f"TMDB path not allowed by relay: {path}")

    if settings.MOCK_UPSTREAM:
        return _mock_response(path, query)

    merged_query = dict(query)
    merged_query["api_key"] = settings.TMDB_API_KEY

    # V5 — every outbound TMDB call is paced through the shared limiter, so
    # the relay as a whole stays under TMDB's per-IP ceiling no matter how
    # many installations happen to be refreshing at once.
    async with upstream_limit.tmdb.slot():
        resp = await client.get(f"{settings.TMDB_BASE_URL}/{path}", params=merged_query)
    upstream_limit.tmdb.note_response(resp.status_code, resp.headers.get("retry-after"))
    return resp


def _mock_response(path: str, query: dict) -> httpx.Response:
    fake_payload = b'{"results": [{"id": 42, "title": "Mock Movie", "release_date": "2026-12-25"}]}'
    return httpx.Response(200, content=fake_payload, request=httpx.Request("GET", "mock://tmdb"))
