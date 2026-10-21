"""
Dashboard show search — V5.

A faithful port of the desktop app's TMDB search (TmdbScraper::searchMedia
plus fetchCreditsForResults), so picking a show to correct on the dashboard
behaves exactly like picking one in the app.

The dashboard originally searched `known_titles`, the relay's own record of
titles it had seen. That worked but was close to useless in practice: it
returns bare names, so a page full of "Daredevil", "Daredevil", "Daredevil"
gives no way to tell the 2015 series from the 1989 film from Born Again.
The app solves that by showing year, director and leading cast — and the
only way to be sure the dashboard agrees with the app about what a query
means is to run the same rules over the same data.

Every filter below mirrors the app's, in the same order and with the same
constants, deliberately including the parts that look arbitrary:

  * "xmen"/"spiderman" are rewritten to their hyphenated forms
  * a trailing 4-digit year is stripped and used to filter instead
  * documentaries, talk shows and reality TV are excluded (genre ids
    99 / 10767 / 10764)
  * non-English originals are excluded
  * podcasts and fan films are excluded by title
  * a result must match on words OR on a punctuation-stripped compaction
  * the title's first meaningful word must equal the query's first word,
    articles skipped, subtitles after " - "/": " also considered
  * multi-word queries must appear as a contiguous prefix, so "the boys"
    cannot match "The Napa Boys"
  * popularity below 2.0 is dropped, which is what keeps fan films out
  * newest year first, then popularity

Credits are fetched for the top few results only. The app fetches them for
every result because it is one user acting on their own quota; here they
come out of the shared credential, so this deliberately spends less.
"""
import re
import unicodedata

import httpx

from app.config import settings
from app import upstream_limit

# Only the top N results get a credits lookup. Each one is a separate TMDB
# request against the shared credential, and the identifying detail matters
# most for the results someone is actually choosing between.
CREDITS_FOR_TOP_N = 8

_ARTICLES = {"the", "a", "an"}
_BLOCKED_GENRES = {10764, 99, 10767}      # reality, documentary, talk
_MIN_POPULARITY = 2.0

# Ported from TmdbScraper's kMarvelAliases. TMDB lists these Netflix-era
# series under "Marvel's ...", so the first-word anchor below would reject
# every one of them: "marvels" is not "daredevil". The alias lets the real
# title through without loosening the anchor for anything else.
#
# Deliberately NOT used to rewrite the query sent upstream — searching
# "punisher" should still return The Punisher (1989) too.
_MARVEL_ALIASES = {
    "daredevil":               "Marvel's Daredevil",
    "the punisher":            "Marvel's The Punisher",
    "punisher":                "Marvel's The Punisher",
    "jessica jones":           "Marvel's Jessica Jones",
    "luke cage":               "Marvel's Luke Cage",
    "iron fist":               "Marvel's Iron Fist",
    "the defenders":           "Marvel's The Defenders",
    "runaways":                "Marvel's Runaways",
    "cloak and dagger":        "Marvel's Cloak & Dagger",
    "cloak & dagger":          "Marvel's Cloak & Dagger",
    "inhumans":                "Marvel's Inhumans",
    "agent carter":            "Marvel's Agent Carter",
    "agents of shield":        "Marvel's Agents of S.H.I.E.L.D.",
    "agents of s.h.i.e.l.d.":  "Marvel's Agents of S.H.I.E.L.D.",
    "ultimate spider-man":     "Marvel's Ultimate Spider-Man",
    "ultimate spiderman":      "Marvel's Ultimate Spider-Man",
    "marvels avengers":        "Marvel's Avengers",
}


def _strip_accents(s: str) -> str:
    d = unicodedata.normalize("NFD", s)
    return "".join(c for c in d if unicodedata.category(c) != "Mn")


def _compact(s: str) -> str:
    """Lowercase, accent- and punctuation-free. 'Spider-Man' -> 'spiderman'."""
    return "".join(c for c in _strip_accents(s).lower() if c.isalnum())


def _words(s: str) -> list[str]:
    """Word list with hyphens/underscores treated as spaces, punctuation dropped."""
    replaced = s.replace("-", " ").replace("_", " ")
    out = []
    for w in _strip_accents(replaced).lower().split():
        clean = "".join(c for c in w if c.isalnum())
        if clean:
            out.append(clean)
    return out


def _normalise_query(q: str) -> tuple[str, int]:
    """Apply the app's query rewrites and pull off a trailing year."""
    q = q.strip()
    q = re.sub(r"\bxmen\b", "x-men", q, flags=re.I)
    q = re.sub(r"\bspiderman\b", "spider-man", q, flags=re.I)

    year_filter = 0
    m = re.search(r"\b(19\d{2}|20\d{2})$", q)
    if m:
        year_filter = int(m.group(1))
        q = q[:m.start()].strip()
    return q, year_filter


def _passes_title_rules(raw_title: str, query_words: list[str],
                        query_compact: str, marvel_title: str = "") -> bool:
    """The app's word-match / first-word anchor / prefix-sequence chain."""
    title_words = _words(raw_title)

    word_match = all(qw in title_words for qw in query_words)
    compact_match = query_compact in _compact(raw_title)
    if not word_match and not compact_match:
        return False
    if not query_words:
        return True

    query_starts_with_article = query_words[0] in _ARTICLES

    anchored = list(title_words)
    if not query_starts_with_article and anchored and anchored[0] in _ARTICLES:
        anchored.pop(0)

    # A Marvel alias hit is accepted outright: the title genuinely starts
    # with "Marvel's", which no anchor rule can accommodate.
    marvel_match = bool(marvel_title) and raw_title.lower() == marvel_title.lower()
    first_word_ok = marvel_match or (bool(anchored) and anchored[0] == query_words[0])

    # V5 — the subtitle fallback is deliberately gone; see the same note in
    # TmdbScraper::searchMedia. It let the part after a ":" satisfy the
    # anchor, which made matching much looser ("daredevil" returning "Super
    # Dave: Daredevil for Hire"). A title must begin with what you typed.
    # "spiderman brand new day" still finds "Spider-Man: Brand New Day"
    # through the ordinary prefix rule.
    if not first_word_ok:
        return False

    # Contiguous prefix, so "the boys" cannot match "The Napa Boys".
    # Skipped when only the compact form matched, since hyphen splitting
    # makes the word lists legitimately unequal.
    if word_match and not marvel_match and len(query_words) > 1:
        for i, qw in enumerate(query_words):
            if i >= len(anchored) or anchored[i] != qw:
                return False
    return True


async def search(client: httpx.AsyncClient, raw_query: str,
                 tv_only: bool = False) -> list[dict]:
    """Run the app's search and return picker-shaped rows.

    tv_only drops films. The dashboard sets it because corrections only
    apply to episode air times — no source publishes a per-title time for a
    film, so the app's own default already handles them and there is
    nothing here that could correct one.
    """
    query, year_filter = _normalise_query(raw_query)
    if len(query) < 2:
        return []

    query_words = _words(query)
    query_compact = _compact(query)
    marvel_title = _MARVEL_ALIASES.get(query.strip().lower(), "")

    combined = []
    for page in (1, 2):          # the app fetches two pages before filtering
        async with upstream_limit.tmdb.slot():
            resp = await client.get(
                f"{settings.TMDB_BASE_URL}/search/multi",
                params={"api_key": settings.TMDB_API_KEY, "query": query,
                        "include_adult": "false", "page": page},
            )
        upstream_limit.tmdb.note_response(resp.status_code,
                                          resp.headers.get("retry-after"))
        if resp.status_code != 200:
            break
        combined.extend(resp.json().get("results", []))

    results, seen = [], set()
    for o in combined:
        kind = o.get("media_type")
        if kind not in ("movie", "tv"):
            continue
        if tv_only and kind != "tv":
            continue

        raw_title = o.get("title") or o.get("name") or ""
        if not raw_title:
            continue
        low = raw_title.lower()
        if "podcast" in low or "fan film" in low or "fan-made" in low:
            continue
        if _BLOCKED_GENRES & set(o.get("genre_ids") or []):
            continue
        if o.get("original_language") != "en":
            continue
        if not _passes_title_rules(raw_title, query_words, query_compact, marvel_title):
            continue

        date_str = o.get("release_date") or o.get("first_air_date") or ""
        year = int(date_str[:4]) if date_str[:4].isdigit() else 0
        if year_filter and year != year_filter:
            continue

        popularity = float(o.get("popularity") or 0.0)
        if popularity < _MIN_POPULARITY:
            continue

        tmdb_id = o.get("id")
        if tmdb_id in seen:
            continue
        seen.add(tmdb_id)

        results.append({
            "tmdb_id":    tmdb_id,
            "media_type": kind,
            "title":      raw_title,
            "year":       year,
            "popularity": popularity,
            "director":   "",
            "cast":       "",
        })

    # Newest first, then popularity within a year; undated entries sink.
    results.sort(key=lambda r: (r["year"] or -1, r["popularity"]), reverse=True)

    for r in results[:CREDITS_FOR_TOP_N]:
        await _attach_credits(client, r)
    return results


async def _attach_credits(client: httpx.AsyncClient, row: dict) -> None:
    """Director (or creator, for TV) plus the three leading cast members."""
    async with upstream_limit.tmdb.slot():
        resp = await client.get(
            f"{settings.TMDB_BASE_URL}/{row['media_type']}/{row['tmdb_id']}/credits",
            params={"api_key": settings.TMDB_API_KEY},
        )
    upstream_limit.tmdb.note_response(resp.status_code, resp.headers.get("retry-after"))
    if resp.status_code != 200:
        return

    data = resp.json()
    # TV has no "Director" in the useful sense — the app looks for the
    # creator first and settles for an executive producer.
    jobs = (["Director"] if row["media_type"] == "movie"
            else ["Creator", "Series Creator", "Executive Producer"])
    for job in jobs:
        match = next((c for c in data.get("crew", []) if c.get("job") == job), None)
        if match:
            row["director"] = match.get("name", "")
            break

    row["cast"] = ", ".join(c.get("name", "") for c in data.get("cast", [])[:3])
