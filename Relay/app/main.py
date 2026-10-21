"""
Media Countdowns Relay — a small proxy that sits between distributed
copies of the Media Countdowns desktop app and TMDB/IGDB.

Design in one sentence: the client sends the exact same kind of request
it would send TMDB/IGDB directly, just to this relay instead and with a
shared-secret token in place of a real API key — the relay validates
that token, injects the REAL credentials, forwards the request upstream,
and returns the response byte-for-byte. This means the desktop app's
existing TMDB/IGDB response-parsing code doesn't need to change at all;
only where it sends the request and how it authenticates does.

Run with:  uvicorn app.main:app --host 0.0.0.0 --port 8080
"""
import httpx
import re
from contextlib import asynccontextmanager
from fastapi import FastAPI, Depends, Request, Response, Header, HTTPException
from fastapi.responses import JSONResponse
from starlette.middleware.sessions import SessionMiddleware
from app.auth import verify_client, ClientIdentity
from app.config import settings
from app import tmdb_proxy, igdb_proxy, cache, rate_limit, db, describe, live_log, titles, refresh_batch

_http_client: httpx.AsyncClient | None = None


def _log_and_broadcast(message: str):
    db.add_console_log(message)
    live_log.broadcast(message)


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _http_client
    db.init_db()
    _http_client = httpx.AsyncClient(timeout=15.0)
    yield
    await _http_client.aclose()


app = FastAPI(title="Media Countdowns Relay", version="0.0.11", lifespan=lifespan)
app.add_middleware(SessionMiddleware, secret_key=settings.SESSION_SECRET)

from app.admin import router as admin_router
app.include_router(admin_router)


@app.get("/health")
async def health():
    # Deliberately unauthenticated — lets you (or a monitoring tool)
    # confirm the relay is up without needing the shared secret.
    return {"status": "ok"}


@app.get("/motd")
async def motd():
    # Deliberately unauthenticated and separate from the admin
    # dashboard's password — this is meant for every app installation to
    # read, not just you. Not yet consumed by the desktop app itself;
    # see the dashboard template for that note.
    return JSONResponse({"message": db.get_motd()})


@app.get("/version")
async def version():
    # Deliberately unauthenticated, same reasoning as /motd above — every
    # installation needs to be able to check this, not just you.
    major, minor, patch = db.get_app_version()
    return JSONResponse({"version": f"{major}.{minor}.{patch}"})


def _handle_tmdb_completion(user_label: str, description: str, category: str, tile_key, ok: bool,
                             from_cache: bool, upstream_status: int | None):
    # Searches (including curated-collection results, which are now part
    # of the search flow itself): a single, deliberate action — logged
    # individually and immediately, same as always.
    if category == "search":
        if ok:
            suffix = " (from cache)" if from_cache else ""
            _log_and_broadcast(f"Sent ({user_label}) {description}{suffix}")
        else:
            _log_and_broadcast(f"Failed to fetch {description} for ({user_label}) (upstream status {upstream_status})")
        db.bump_stat("tiles_fetched_ever")
        return

    # Credits and refresh both batch (see refresh_batch.py), but stay in
    # entirely separate buckets — a burst of credits lookups for search
    # results is not a tile being refreshed, and must never be labeled
    # as one.
    source = "tmdb_movie" if tile_key[0] == "movie" else "tmdb_tv"
    name = db.lookup_title(source, tile_key[1])
    refresh_batch.add_item(user_label, category, tile_key[0], tile_key[1], name, ok, from_cache,
                           _log_and_broadcast, upstream_status)
    db.bump_stat("tiles_fetched_ever" if category == "credits" else "tiles_refreshed_ever")


@app.get("/tmdb/{path:path}")
async def tmdb_route(path: str, request: Request, identity: ClientIdentity = Depends(verify_client)):
    rate_limit.check(identity.client_id)
    user_label = identity.label

    query = dict(request.query_params)
    description = describe.describe_tmdb_request(path, {k: [v] for k, v in query.items()})

    if describe.is_tmdb_search_request(path):
        category = "search"
        tile_key = None
    elif describe.is_tmdb_credits_request(path):
        category = "credits"
        tile_key = describe.tmdb_tile_key(path)
    else:
        category = "refresh"
        tile_key = describe.tmdb_tile_key(path)

    if category == "search":
        _log_and_broadcast(f"({user_label}) requested {description}")

    key = cache.make_key("GET", f"tmdb/{path}?{request.url.query}", None)
    cached = cache.get(key)
    if cached is not None:
        _handle_tmdb_completion(user_label, description, category, tile_key, True, True, None)
        return Response(content=cached["body"], status_code=cached["status"], media_type="application/json")

    resp = await tmdb_proxy.forward(path, query, _http_client)
    if resp.status_code == 200:
        titles.remember_from_tmdb_response(path, resp.content)
        # Description may now resolve to a real name if this response
        # itself revealed it and we hadn't seen it before.
        description = describe.describe_tmdb_request(path, {k: [v] for k, v in query.items()})
        ttl = settings.CACHE_TTL_SEARCH_SECONDS if category == "search" else settings.CACHE_TTL_DETAILS_SECONDS
        cache.set(key, "GET", f"tmdb/{path}", resp.content, resp.status_code, ttl)
        _handle_tmdb_completion(user_label, description, category, tile_key, True, False, None)
    else:
        _handle_tmdb_completion(user_label, description, category, tile_key, False, False, resp.status_code)
    return Response(content=resp.content, status_code=resp.status_code, media_type="application/json")


def _handle_igdb_completion(user_label: str, description: str, tile_key, ok: bool,
                             from_cache: bool, upstream_status: int | None):
    if tile_key is None:
        if ok:
            suffix = " (from cache)" if from_cache else ""
            _log_and_broadcast(f"Sent ({user_label}) {description}{suffix}")
        else:
            _log_and_broadcast(f"Failed to fetch {description} for ({user_label}) (upstream status {upstream_status})")
        db.bump_stat("tiles_fetched_ever")
        return

    name = db.lookup_title("igdb_game", tile_key[1])
    refresh_batch.add_item(user_label, "refresh", tile_key[0], tile_key[1], name, ok, from_cache,
                           _log_and_broadcast, upstream_status)
    db.bump_stat("tiles_refreshed_ever")


@app.get("/overrides")
async def overrides_route(identity: ClientIdentity = Depends(verify_client)):
    """
    V5 — per-show air-time corrections, applied by every installation.

    Some services aren't covered by any source: TVmaze records no air time
    for Disney+, so a show that really drops at 9 PM Eastern falls back to
    the app's generic default and there is no API anywhere that knows
    better. This is the one place a human answer gets published to
    everyone at once.

    Deliberately tiny and unconditional — a few dozen rows, fetched once
    per refresh. No caching layer: it's small, it changes rarely, and a
    stale correction is exactly the thing this endpoint exists to fix.
    """
    rate_limit.check(identity.client_id)
    return JSONResponse({"overrides": db.overrides_all()})


@app.post("/report-airtime")
async def report_airtime_route(request: Request,
                                identity: ClientIdentity = Depends(verify_client)):
    """
    V5 — "this air time looks wrong", one click from the app.

    Carries no free text: just which show. That keeps it a signal rather
    than a support inbox, and means there is nothing to moderate.

    One row per installation per show, so repeated clicking can't inflate a
    count. A repeat inside the cooldown is answered 200 with
    counted=false — it isn't an error, and the app shouldn't show one.
    """
    rate_limit.check(identity.client_id)
    body = await request.json()
    show_key = str(body.get("show_key") or "").strip()
    if not show_key:
        return JSONResponse({"detail": "show_key is required"}, status_code=400)

    title  = str(body.get("title") or "").strip()
    source = str(body.get("source") or "").strip()

    counted = db.report_add(show_key, identity.client_id, title, source)
    if counted:
        _log_and_broadcast(
            f"({identity.label}) reported a wrong air time for "
            f"{title or show_key}")
    return JSONResponse({"ok": True, "counted": counted})


@app.post("/register")
async def register_route(request: Request, authorization: str = Header(default="")):
    """
    V0.0.13 — hand an installation an id it can prove it was given.

    Deliberately not behind verify_client: a caller has no id yet, which is
    the entire reason it is here. The shared secret is still required, so this
    is no more open than any other endpoint.

    The id is signed by the relay, so one that was typed by hand or copied
    from somebody else simply fails verification and gets replaced by a fresh
    one on the next request. That is what stops the installation count being
    something anybody can inflate, deliberately or — as happened here — by
    passing a made-up X-Client-ID while debugging.

    Registering does NOT create the installation row. That happens on the
    first real request the new id is used for, so an id handed out and never
    used doesn't show up as an installation that never existed.
    """
    if not settings.RELAY_SHARED_SECRET:
        raise HTTPException(status_code=500, detail="Relay has no shared secret configured")
    if authorization != f"Bearer {settings.RELAY_SHARED_SECRET}":
        raise HTTPException(status_code=401, detail="Invalid or missing relay token")
    # Limited by address, since there is no client id yet — see rate_limit.py
    # for why this route is the one that could otherwise defeat every limit.
    rate_limit.check_register(rate_limit.client_address(request))
    return JSONResponse({"client_id": db.issue_client_id()})


@app.post("/window-superseded")
async def window_superseded_route(request: Request,
                                  identity: ClientIdentity = Depends(verify_client)):
    """
    V5.4.6 — an app installation found a real published date for a show whose
    correction only guessed at a window.

    The window has done its job and must now go, for everyone. Leaving it would
    do two wrong things: keep overriding the real date that has just arrived,
    and then reassert the same month against the NEXT season, which may return
    at a completely different time of year.

    Only the window is cleared — an air-time correction is about a slot the
    show airs in, which doesn't change because a season did, so it survives.

    Any authenticated installation may report this, which is safe because it
    can only ever remove a guess that real data has already replaced, never
    change one and never touch an air time. The date is required and has to
    parse, so a malformed or empty report does nothing.
    """
    rate_limit.check(identity.client_id)
    body = await request.json()

    show_key = str(body.get("show_key") or "").strip()
    if not show_key:
        return JSONResponse({"detail": "show_key is required"}, status_code=400)

    real_date = str(body.get("real_date") or "").strip()
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", real_date):
        return JSONResponse({"detail": "real_date must be YYYY-MM-DD"}, status_code=400)

    cleared = db.override_clear_window(show_key)
    if cleared:
        title = str(body.get("title") or "").strip() or show_key
        _log_and_broadcast(
            f"({identity.label}) {title} now has a real date "
            f"({real_date}) — its window correction was removed")
    return JSONResponse({"ok": True, "cleared": cleared})


@app.post("/feedback")
async def feedback_route(request: Request,
                         identity: ClientIdentity = Depends(verify_client)):
    """
    V5.4.3 — free-text bug reports and feature ideas, written in the app.

    Deliberately different from /report-airtime, which carries no text and is
    a one-click signal about a specific show. This is a mailbox: the whole
    point is that someone can describe something the app has no button for.

    Anonymous by design. The only identifier stored is the same random
    per-install id every request already carries, which is what makes the
    cooldown possible and lets a follow-up be recognised as the same person —
    it is not tied to any account, name or address, and nothing here asks for
    one. No reply channel exists on purpose: adding one would mean holding
    contact details for a free app that currently holds none.
    """
    rate_limit.check(identity.client_id)
    body = await request.json()

    kind = str(body.get("kind") or "").strip().lower()
    if kind not in db.FEEDBACK_KINDS:
        return JSONResponse({"detail": "kind must be 'bug' or 'request'"}, status_code=400)

    text = str(body.get("body") or "").strip()
    if not text:
        return JSONResponse({"detail": "body is required"}, status_code=400)
    if len(text) > db.FEEDBACK_MAX_LENGTH:
        return JSONResponse(
            {"detail": f"body must be {db.FEEDBACK_MAX_LENGTH} characters or fewer"},
            status_code=400)

    app_version = str(body.get("app_version") or "").strip()

    accepted = db.feedback_add(kind, identity.client_id, text, app_version)
    if accepted:
        label = "a bug report" if kind == "bug" else "a feature request"
        _log_and_broadcast(f"({identity.label}) sent {label}")
    return JSONResponse({"ok": True, "accepted": accepted})


@app.post("/igdb/{path:path}")
async def igdb_route(path: str, request: Request, identity: ClientIdentity = Depends(verify_client)):
    rate_limit.check(identity.client_id)
    user_label = identity.label

    body = await request.body()
    description = describe.describe_igdb_request(body)
    tile_key = describe.igdb_tile_key(body) if not describe.is_igdb_search_request(body) else None

    if tile_key is None:
        _log_and_broadcast(f"({user_label}) requested {description}")

    key = cache.make_key("POST", f"igdb/{path}", body)
    cached = cache.get(key)
    if cached is not None:
        _handle_igdb_completion(user_label, description, tile_key, True, True, None)
        return Response(content=cached["body"], status_code=cached["status"], media_type="application/json")

    resp = await igdb_proxy.forward(path, body, _http_client)
    if resp.status_code == 200:
        titles.remember_from_igdb_response(resp.content)
        description = describe.describe_igdb_request(body)
        ttl = settings.CACHE_TTL_SEARCH_SECONDS if tile_key is None else settings.CACHE_TTL_DETAILS_SECONDS
        cache.set(key, "POST", f"igdb/{path}", resp.content, resp.status_code, ttl)
        _handle_igdb_completion(user_label, description, tile_key, True, False, None)
    else:
        _handle_igdb_completion(user_label, description, tile_key, False, False, resp.status_code)
    return Response(content=resp.content, status_code=resp.status_code, media_type="application/json")
