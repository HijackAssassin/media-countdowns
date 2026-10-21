"""
Learns real titles from TMDB/IGDB responses the relay is already
forwarding — no extra API calls needed. A detail-fetch response
includes its own title; a search response includes one per result.
Feeds app.db's known_titles table, which describe.py then consults to
show "The Batman" instead of "movie #1091" once it's been seen at least
once.

Deliberately skips season/credits responses for the SHOW's own name —
those bodies only describe the season/credits themselves (e.g. a
season's own "name" is "Season 1", not the show's name), so pulling a
"name" field from them would silently store the wrong thing. The show's
real name normally arrives from a separate plain tv/{id} detail fetch
in the same batch instead.
"""
import json
from app import db


def remember_from_tmdb_response(path: str, body: bytes):
    try:
        data = json.loads(body)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return

    if path.startswith("search/multi"):
        for r in data.get("results", []):
            media_type = r.get("media_type")
            item_id = r.get("id")
            if item_id is None:
                continue
            if media_type == "movie" and r.get("title"):
                db.remember_title("tmdb_movie", item_id, r["title"])
            elif media_type == "tv" and r.get("name"):
                db.remember_title("tmdb_tv", item_id, r["name"])
        return

    if path.startswith("movie/") and path.count("/") == 1:
        item_id = path.split("/", 1)[1]
        if data.get("title"):
            db.remember_title("tmdb_movie", item_id, data["title"])
        return

    if path.startswith("collection/"):
        item_id = path.split("/", 1)[1]
        if data.get("name"):
            db.remember_title("tmdb_collection", item_id, data["name"])
        return

    if path.startswith("tv/") and path.count("/") == 1:
        item_id = path.split("/", 1)[1]
        if data.get("name"):
            db.remember_title("tmdb_tv", item_id, data["name"])
        return

    # Season/credits responses intentionally not handled here — see
    # module docstring.


def remember_from_igdb_response(body: bytes):
    try:
        data = json.loads(body)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return
    if not isinstance(data, list):
        return
    for item in data:
        if isinstance(item, dict) and item.get("id") is not None and item.get("name"):
            db.remember_title("igdb_game", item["id"], item["name"])
