"""
IGDB proxy.

IGDB authenticates via Twitch's OAuth2 client-credentials flow: you
exchange a Client ID + Client Secret for a short-lived access token,
then send that token (plus the Client ID) with every actual API call.

The whole point of this relay is that the desktop app never sees any of
that — it just sends its request here, and this module handles
fetching/caching/refreshing the Twitch token behind the scenes,
transparently to the client.
"""
import time
import httpx
from fastapi import HTTPException
from app.config import settings
from app import upstream_limit

_token: str | None = None
_token_expiry_epoch: float = 0

# v1 — the exact set of IGDB endpoints Media Countdowns actually calls.
# Deliberately a whitelist, not "forward anything" — this is what keeps
# the relay from becoming a general-purpose open proxy to IGDB using
# your credentials for whatever anyone wants.
ALLOWED_IGDB_PATHS = {"games"}


async def _get_token(client: httpx.AsyncClient) -> str:
    global _token, _token_expiry_epoch
    if _token and time.time() < _token_expiry_epoch - 60:
        return _token

    if settings.MOCK_UPSTREAM:
        _token = "mock-igdb-token"
        _token_expiry_epoch = time.time() + 3600
        return _token

    resp = await client.post(
        settings.TWITCH_TOKEN_URL,
        params={
            "client_id": settings.IGDB_CLIENT_ID,
            "client_secret": settings.IGDB_CLIENT_SECRET,
            "grant_type": "client_credentials",
        },
    )
    resp.raise_for_status()
    data = resp.json()
    _token = data["access_token"]
    _token_expiry_epoch = time.time() + data.get("expires_in", 3600)
    return _token


async def forward(path: str, body: bytes, client: httpx.AsyncClient) -> httpx.Response:
    if path not in ALLOWED_IGDB_PATHS:
        raise HTTPException(status_code=404, detail=f"IGDB path not allowed by relay: {path}")

    if settings.MOCK_UPSTREAM:
        return _mock_response(path, body)

    token = await _get_token(client)

    # V5 — IGDB is the strict one: 4 requests/second and no more than 8 open
    # at a time, enforced per credential rather than per user. Since every
    # installation's game lookups funnel through this single set of
    # credentials, this limiter is the only thing standing between a busy
    # moment and a 429 that would affect everyone at once.
    async with upstream_limit.igdb.slot():
        resp = await client.post(
            f"{settings.IGDB_BASE_URL}/{path}",
            content=body,
            headers={
                "Client-ID": settings.IGDB_CLIENT_ID,
                "Authorization": f"Bearer {token}",
                "Content-Type": "text/plain",
            },
        )
    upstream_limit.igdb.note_response(resp.status_code, resp.headers.get("retry-after"))
    return resp


def _mock_response(path: str, body: bytes) -> httpx.Response:
    # Used only when MOCK_UPSTREAM=true (no real IGDB credentials needed)
    # — lets the relay's own routing/auth/caching logic be exercised and
    # tested without real IGDB access.
    fake_payload = b'[{"id": 1, "name": "Mock Game", "first_release_date": 1700000000}]'
    return httpx.Response(200, content=fake_payload, request=httpx.Request("POST", "mock://igdb"))
