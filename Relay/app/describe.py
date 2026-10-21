"""
Turns a raw TMDB/IGDB request into a short, human-readable description
for the console log — e.g. 'search results for "Minecraft"' rather than
the raw path/query string. Kept as its own module since this is pure,
easily-testable parsing logic with no side effects, separate from the
actual request-forwarding code in tmdb_proxy.py/igdb_proxy.py.

Uses app.db's known_titles table (populated by titles.py) to show real
names instead of bare IDs once a title has actually been seen at least
once — falls back to "#12345" for anything not yet known.
"""
import re
from app import db


def describe_tmdb_request(path: str, query: dict) -> str:
    if path.startswith("search/multi"):
        term = query.get("query", [""])
        term = term[0] if isinstance(term, list) else term
        return f'search results for "{term}"'

    if path.startswith("collection/"):
        item_id = path.split("/", 1)[1]
        name = db.lookup_title("tmdb_collection", item_id)
        return f'collection results for "{name}"' if name else f"collection results for #{item_id}"

    m = re.match(r"^(movie|tv)/(\d+)/season/(\d+)", path)
    if m:
        kind, show_id, season = m.groups()
        label = _resolve(kind, show_id)
        return f"season {season} details for {label}"

    m = re.match(r"^(movie|tv)/(\d+)/credits", path)
    if m:
        kind, item_id = m.groups()
        label = _resolve(kind, item_id)
        return f"credits for {label}"

    m = re.match(r"^(movie|tv)/(\d+)", path)
    if m:
        kind, item_id = m.groups()
        label = _resolve(kind, item_id)
        return f"details for {label}"

    return f"data from {path}"


def describe_igdb_request(body: bytes) -> str:
    text = body.decode("utf-8", errors="replace")

    m = re.search(r'search\s+"([^"]*)"', text)
    if m:
        return f'search results for "{m.group(1)}"'

    m = re.search(r"where\s+id\s*=\s*(\d+)", text)
    if m:
        item_id = m.group(1)
        name = db.lookup_title("igdb_game", item_id)
        return f'details for "{name}"' if name else f"details for game #{item_id}"

    return "game data"


def _resolve(kind: str, item_id: str) -> str:
    source = "tmdb_movie" if kind == "movie" else "tmdb_tv"
    name = db.lookup_title(source, item_id)
    if name:
        return f'"{name}"'
    label = "movie" if kind == "movie" else "show"
    return f"{label} #{item_id}"


def is_tmdb_search_request(path: str) -> bool:
    # collection/{id} is now part of the curated-collection search path
    # itself (the client calls it directly, in place of search/multi,
    # when a query matches one of its hand-picked triggers) — treated
    # the same as a search: immediate, individual logging, never batched.
    return path.startswith("search/multi") or path.startswith("collection/")


def is_tmdb_credits_request(path: str) -> bool:
    # Credits are only ever fetched for currently-displayed search
    # results (see fetchCreditsForResults in tmdbscraper.cpp) — never as
    # part of refreshing an existing saved tile. Classified separately
    # so a burst of these doesn't get mislabeled as "Refreshed N tiles"
    # in the console — nothing is actually being refreshed.
    return bool(re.match(r"^(movie|tv)/\d+/credits", path))


def is_igdb_search_request(body: bytes) -> bool:
    return b'search "' in body


def tmdb_tile_key(path: str):
    """Returns (kind, id) for a plain-details or season-details request,
    so a show's separate plain-details and season-details calls resolve
    to the SAME tile key and get merged in a refresh batch, rather than
    counted as two different tiles. Returns None for search and credits
    requests — callers should check those first."""
    m = re.match(r"^(movie|tv)/(\d+)", path)
    if not m:
        return None
    kind, item_id = m.groups()
    return ("movie" if kind == "movie" else "show", item_id)


def igdb_tile_key(body: bytes):
    m = re.search(r"where\s+id\s*=\s*(\d+)", body.decode("utf-8", errors="replace"))
    return ("game", m.group(1)) if m else None
