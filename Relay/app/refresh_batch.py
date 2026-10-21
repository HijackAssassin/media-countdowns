"""
Batches bulk burst activity — many detail/season/credits fetches from
the same user in quick succession — into one combined console line
instead of dozens of individual ones. Two separate kinds of burst get
batched, kept entirely apart from each other:

  "refresh" — the app's startup refresh of every saved tile's details/
              dates. Summarized as "(User N) Refreshed 4 tiles (...)".
  "credits" — cast/crew lookups for whatever's currently showing in a
              search results list (see fetchCreditsForResults in
              tmdbscraper.cpp) — NOT a tile being refreshed at all, just
              supplementary info for something being searched right now.
              Summarized as "(User N) Loaded cast & crew for 4 search
              results" — kept in its own category specifically so it's
              never confused with an actual tile refresh.

Uses a debounce: each new completed item for a user+kind (re)starts a
short timer; the summary only actually gets emitted once that user has
gone quiet for FLUSH_DELAY_SECONDS on that particular kind. A single
tile can generate more than one "refresh" request (e.g. a show's plain
details call AND its season call) — these are merged by (kind, id) into
one entry, so the summary counts tiles, not individual requests.

Only used for detail/season/credits-type requests — searches (including
the curated-collection results, which are now part of the search flow
itself) are a single, deliberate user action and stay logged
individually and immediately, unbatched.
"""
import asyncio

FLUSH_DELAY_SECONDS = 2.0

_KIND_LABELS = {"movie": "Movie", "show": "Show", "game": "Game"}

_pending: dict[tuple[str, str], dict] = {}


def add_item(user_label: str, batch_kind: str, kind: str, item_id: str, name: str | None,
             ok: bool, from_cache: bool, emit_fn, status: int | None = None):
    """batch_kind is "refresh" or "credits" — keeps the two burst types
    entirely separate so they never merge into one confusing summary."""
    pending_key = (user_label, batch_kind)
    entry = _pending.setdefault(pending_key, {"items": {}, "task": None})
    key = (kind, item_id)
    existing = entry["items"].get(key)
    if existing is None:
        entry["items"][key] = {"name": name, "ok": ok, "from_cache": from_cache,
                               "status": status}
    else:
        # Merge: keep whichever name we have (prefer a newly-learned one
        # over none); only stay "ok" if every sub-request for this same
        # item succeeded; only stay "from_cache" if every sub-request
        # for it was.
        if name and not existing["name"]:
            existing["name"] = name
        existing["ok"] = existing["ok"] and ok
        # Keep the first upstream status that actually failed — that is the one
        # worth reporting, not whatever a later sub-request happened to return.
        if not ok and existing.get("status") is None:
            existing["status"] = status
        existing["from_cache"] = existing["from_cache"] and from_cache

    if entry["task"] is not None:
        entry["task"].cancel()
    entry["task"] = asyncio.ensure_future(_flush_after_delay(pending_key, emit_fn))


async def _flush_after_delay(pending_key: tuple[str, str], emit_fn):
    try:
        await asyncio.sleep(FLUSH_DELAY_SECONDS)
    except asyncio.CancelledError:
        return  # a newer item arrived and restarted the timer; this one's superseded

    entry = _pending.pop(pending_key, None)
    if not entry or not entry["items"]:
        return
    user_label, batch_kind = pending_key
    emit_fn(_format_summary(user_label, batch_kind, entry["items"]))


def _label_for(kind: str, item_id: str, name: str | None) -> str:
    return f'"{name}"' if name else f"{kind} #{item_id}"


def _format_summary(user_label: str, batch_kind: str, items: dict) -> str:
    ok_items = [(k, v) for k, v in items.items() if v["ok"]]
    failed_items = [(k, v) for k, v in items.items() if not v["ok"]]
    total = len(items)
    plural = "s" if total != 1 else ""

    if batch_kind == "credits":
        msg = f"({user_label}) Loaded cast & crew for {total} search result{plural}"
    else:
        counts: dict[str, int] = {}
        for k, v in ok_items:
            counts[k[0]] = counts.get(k[0], 0) + 1
        # Stable, readable order: movies, then shows, then games, skipping
        # any kind with zero of them in this particular batch.
        breakdown_parts = []
        for kind in ("movie", "show", "game"):
            n = counts.get(kind, 0)
            if n == 0:
                continue
            label = _KIND_LABELS[kind]
            breakdown_parts.append(f"{n} {label}{'s' if n != 1 else ''}")
        breakdown = f" ({', '.join(breakdown_parts)})" if breakdown_parts else ""
        msg = f"({user_label}) Refreshed {total} tile{plural}{breakdown}"

    if ok_items and all(v["from_cache"] for k, v in ok_items):
        msg += " (all from cache)"

    if failed_items:
        # V5.4.22 — say WHY. A summary that only says "3 failed" is a dead end
        # when somebody asks what went wrong days later: the individual-request
        # log line already carried the upstream status, but the batched one
        # threw it away, so the three tiles failing here on 08-13 could not be
        # diagnosed afterwards at all.
        failed_parts = [
            _label_for(k[0], k[1], v["name"])
            + (f" ({v['status']})" if v.get("status") else "")
            for k, v in failed_items
        ]
        msg += f" — {len(failed_items)} failed: " + ", ".join(failed_parts)
    return msg
