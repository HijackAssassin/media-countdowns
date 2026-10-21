"""
Admin dashboard: login, usage stats, message-of-the-day control.

Deliberately separate from the client-facing API auth (app/auth.py) —
that shared secret is what desktop app installations use to make
requests; this password is what lets *you* look at the dashboard. They
should never be the same value.
"""
from fastapi import APIRouter, Request, Form, Depends, HTTPException
from fastapi.responses import HTMLResponse, RedirectResponse, StreamingResponse, JSONResponse
from fastapi.templating import Jinja2Templates
from jinja2 import Environment, FileSystemLoader
import os
import re
from app.config import settings
from app import db, live_log, stats_stream, show_search

router = APIRouter()
# cache_size=0 sidesteps a Jinja2/Starlette template-cache interaction
# that otherwise raised spurious errors here — not needed anyway for a
# low-traffic admin-only page.
_jinja_env = Environment(loader=FileSystemLoader("app/templates"), cache_size=0)
templates = Jinja2Templates(env=_jinja_env)


@router.get("/admin", response_class=HTMLResponse)
async def dashboard(request: Request):
    if not request.session.get("admin_ok"):
        return RedirectResponse("/admin/login")

    stats = db.dashboard_stats()
    total_lookups = stats["cache_hits"] + stats["cache_misses"]
    hit_rate = round(100 * stats["cache_hits"] / total_lookups, 1) if total_lookups else 0.0

    return templates.TemplateResponse(request, "dashboard.html", {
        "stats": stats,
        "hit_rate": hit_rate,
        "db_size_mb": round(stats["db_file_bytes"] / (1024 * 1024), 2),
        "cache_size_kb": round(stats["cache_bytes"] / 1024, 1),
        "motd": db.get_motd(),
        "app_version": db.get_app_version(),
        # V5.4.13 — which form was just saved, not merely "something was".
        # Both forms redirected to ?saved=1 and both templates tested it, so
        # saving the MOTD put a green "Saved." under App Version as well, and
        # it stayed there for as long as the URL kept the parameter — which
        # looked like both fields permanently claiming to be saved. The page
        # strips the parameter once shown, so a reload doesn't repeat it.
        "saved": request.query_params.get("saved", ""),
        "db_path": os.path.abspath(db.DB_PATH),
    })


@router.get("/admin/dashboard/stream")
async def dashboard_stream(request: Request):
    # Same Server-Sent Events pattern as the console — pushes the full,
    # current set of numbers the instant any of them changes, instead of
    # the page needing to reload on a timer.
    if not request.session.get("admin_ok"):
        raise HTTPException(status_code=401, detail="Not logged in")

    async def event_stream():
        queue = stats_stream.subscribe()
        try:
            yield "retry: 2000\n\n"
            while True:
                if await request.is_disconnected():
                    break
                payload = await queue.get()
                yield f"data: {payload}\n\n"
        finally:
            stats_stream.unsubscribe(queue)

    return StreamingResponse(event_stream(), media_type="text/event-stream")


@router.get("/admin/console", response_class=HTMLResponse)
async def console(request: Request):
    if not request.session.get("admin_ok"):
        return RedirectResponse("/admin/login")

    import datetime
    entries = db.get_recent_console_log(limit=200)
    for e in entries:
        e["time_str"] = datetime.datetime.fromtimestamp(e["ts"]).strftime("%H:%M:%S")

    return templates.TemplateResponse(request, "console.html", {"entries": entries})


@router.get("/admin/console/stream")
async def console_stream(request: Request):
    # Server-Sent Events — pushes each new log line to the browser the
    # instant it happens, instead of the page needing to reload on a
    # timer. Gated by the same admin login as the console page itself,
    # not a separate, unauthenticated hole into the log.
    if not request.session.get("admin_ok"):
        raise HTTPException(status_code=401, detail="Not logged in")

    async def event_stream():
        queue = live_log.subscribe()
        try:
            yield "retry: 2000\n\n"
            while True:
                if await request.is_disconnected():
                    break
                message = await queue.get()
                yield f"data: {message}\n\n"
        finally:
            live_log.unsubscribe(queue)

    return StreamingResponse(event_stream(), media_type="text/event-stream")


@router.get("/admin/login", response_class=HTMLResponse)
async def login_form(request: Request, error: str = ""):
    return templates.TemplateResponse(request, "login.html", {"error": error})


@router.post("/admin/login")
async def login_submit(request: Request, password: str = Form(...)):
    if not settings.ADMIN_PASSWORD:
        return templates.TemplateResponse(
            request, "login.html",
            {"error": "No ADMIN_PASSWORD configured on the server"},
            status_code=500,
        )
    if password != settings.ADMIN_PASSWORD:
        return templates.TemplateResponse(
            request, "login.html", {"error": "Incorrect password"}, status_code=401
        )
    request.session["admin_ok"] = True
    return RedirectResponse("/admin", status_code=303)


@router.get("/admin/logout")
async def logout(request: Request):
    request.session.clear()
    return RedirectResponse("/admin/login")


@router.post("/admin/motd")
async def set_motd(request: Request, message: str = Form(default="")):
    if not request.session.get("admin_ok"):
        return RedirectResponse("/admin/login")
    db.set_motd(message.strip())
    return RedirectResponse("/admin?saved=motd", status_code=303)


@router.post("/admin/version")
async def set_version(request: Request, major: int = Form(...), minor: int = Form(...), patch: int = Form(...)):
    if not request.session.get("admin_ok"):
        return RedirectResponse("/admin/login")
    # Clamp rather than reject — a stray negative or huge number here
    # should never be able to 500 the whole admin page.
    major = max(0, min(major, 9999))
    minor = max(0, min(minor, 9999))
    patch = max(0, min(patch, 9999))
    db.set_app_version(major, minor, patch)
    return RedirectResponse("/admin?saved=version", status_code=303)


@router.get("/admin/corrections", response_class=HTMLResponse)
async def corrections(request: Request):
    if not request.session.get("admin_ok"):
        return RedirectResponse("/admin/login")
    return templates.TemplateResponse(request, "corrections.html", {})


@router.get("/admin/shows/search")
async def admin_shows_search(request: Request, q: str = ""):
    """
    V5 — find a show to correct, using the desktop app's own search.

    This first searched `known_titles`, the relay's record of titles it had
    already seen. That found things, but returned bare names — a page of
    identical "Daredevil" rows with no way to tell the 2015 series from the
    1989 film from Born Again. show_search.py ports the app's TMDB search
    instead, filters and ordering included, so a result here carries the
    same year, director and cast the app shows and means the same show.
    """
    if not request.session.get("admin_ok"):
        return JSONResponse({"results": []}, status_code=401)

    q = (q or "").strip()
    if len(q) < 2:
        return JSONResponse({"results": []})

    from app import main as _main   # the shared httpx client lives there
    # TV only: corrections set episode air times, and films have no
    # published per-title time to correct.
    rows = await show_search.search(_main._http_client, q, tv_only=True)

    # Attach any correction each result already has, so the row can show it
    # without a second round trip.
    with db.get_conn() as conn:
        existing = {r["show_key"]: r for r in conn.execute(
            "SELECT show_key, " + ", ".join(db.OVERRIDE_COLUMNS) +
            " FROM show_overrides").fetchall()}

    out = []
    for r in rows:
        key = f"tmdb_{r['media_type']}:{r['tmdb_id']}"
        out.append({
            "show_key":  key,
            "source":    f"tmdb_{r['media_type']}",
            "tmdb_id":   r["tmdb_id"],
            "media_type": r["media_type"],
            "title":     r["title"],
            "year":      r["year"],
            "director":  r["director"],
            "cast":      r["cast"],
            # Every correction column, from the one list in db.py — a search
            # hit opens the same editor as an Active row, so it has to carry
            # the same fields or saving from here silently clears the rest.
            **db.override_fields(existing.get(key)),
        })
    return JSONResponse({"results": out})


@router.get("/admin/shows/airtime")
async def admin_show_airtime(request: Request, title: str = "", year: int = 0):
    """
    What air time this show currently has, before any correction.

    The editor used to open blank, which meant deciding whether a time was
    wrong without being shown what it was. TVmaze is the app's source for
    episode times, so this asks it the same question the app would: an empty
    airtime there is exactly why a show falls back to the app's own default.
    """
    if not request.session.get("admin_ok"):
        return JSONResponse({}, status_code=401)
    title = (title or "").strip()
    if not title:
        return JSONResponse({"air_time": "", "source": ""})

    from app import main as _main
    try:
        r = await _main._http_client.get(
            "https://api.tvmaze.com/search/shows",
            params={"q": title},
            headers={"User-Agent": "MediaCountdownsRelay/5.0 (+admin dashboard)"},
        )
        if r.status_code != 200:
            return JSONResponse({"air_time": "", "source": ""})
        hits = r.json()
    except Exception:
        return JSONResponse({"air_time": "", "source": ""})

    # Prefer an exact title match, and the right year when one is known —
    # a franchise can have several entries with the same name.
    best = None
    for h in hits:
        show = h.get("show") or {}
        name = (show.get("name") or "").strip().lower()
        if name != title.strip().lower():
            continue
        premiered = (show.get("premiered") or "")[:4]
        if year and premiered.isdigit() and abs(int(premiered) - year) > 1:
            continue
        best = show
        break
    if best is None and hits:
        best = hits[0].get("show") or {}

    sched = (best or {}).get("schedule") or {}
    air = (sched.get("time") or "").strip()
    net = (best or {}).get("network") or (best or {}).get("webChannel") or {}
    return JSONResponse({
        "air_time": air,
        "source":   net.get("name") or "",
        "matched":  (best or {}).get("name") or "",
    })


@router.get("/admin/overrides")
async def admin_overrides(request: Request):
    """Every correction currently in force, for the Active list."""
    if not request.session.get("admin_ok"):
        return JSONResponse({"overrides": []}, status_code=401)
    rows = db.overrides_all()
    rows.sort(key=lambda r: (r.get("title") or r["show_key"]).lower())
    for r in rows:
        # media_type lets the row render "TV Show"/"Movie" like a search hit.
        r["media_type"] = "tv" if "tv" in (r.get("source") or "") else "movie"
    return JSONResponse({"overrides": rows})


# ── Feedback inboxes (V5.4.3) ───────────────────────────────────────────────
#
# Bugs and Requests are the same page with a different `kind`. They differ
# only in wording and which rows they show, so one template renders both —
# two near-identical templates would drift the moment either was touched.
@router.get("/admin/bugs", response_class=HTMLResponse)
async def admin_bugs(request: Request):
    if not request.session.get("admin_ok"):
        return RedirectResponse("/admin/login")
    return templates.TemplateResponse(request, "feedback.html", {
        "kind": "bug",
        "heading": "Bugs",
        "blurb": "Problems reported from inside the app.",
        "empty": "No bug reports yet.",
        "unread": db.feedback_unread_counts(),
    })


@router.get("/admin/requests", response_class=HTMLResponse)
async def admin_requests(request: Request):
    if not request.session.get("admin_ok"):
        return RedirectResponse("/admin/login")
    return templates.TemplateResponse(request, "feedback.html", {
        "kind": "request",
        "heading": "Requests",
        "blurb": "Ideas and feature suggestions sent from inside the app.",
        "empty": "No requests yet.",
        "unread": db.feedback_unread_counts(),
    })


@router.get("/admin/feedback/list")
async def admin_feedback_list(request: Request, kind: str = "bug"):
    if not request.session.get("admin_ok"):
        return JSONResponse({"entries": []}, status_code=401)
    if kind not in db.FEEDBACK_KINDS:
        return JSONResponse({"entries": []}, status_code=400)
    return JSONResponse({
        "entries": db.feedback_list(kind),
        "unread": db.feedback_unread_counts(),
    })


@router.post("/admin/feedback/read")
async def admin_feedback_read(request: Request, entry_id: int = Form(...),
                              read: int = Form(1)):
    if not request.session.get("admin_ok"):
        return JSONResponse({"ok": False}, status_code=401)
    db.feedback_set_read(entry_id, bool(read))
    return JSONResponse({"ok": True, "unread": db.feedback_unread_counts()})


@router.post("/admin/feedback/todo")
async def admin_feedback_todo(request: Request, entry_id: int = Form(...),
                              todo: int = Form(1)):
    if not request.session.get("admin_ok"):
        return JSONResponse({"ok": False}, status_code=401)
    db.feedback_set_todo(entry_id, bool(todo))
    return JSONResponse({"ok": True, "unread": db.feedback_unread_counts()})


@router.post("/admin/feedback/delete")
async def admin_feedback_delete(request: Request, entry_id: int = Form(...)):
    if not request.session.get("admin_ok"):
        return JSONResponse({"ok": False}, status_code=401)
    db.feedback_delete(entry_id)
    return JSONResponse({"ok": True, "unread": db.feedback_unread_counts()})


@router.get("/admin/reports")
async def admin_reports(request: Request):
    """Reported shows for the dashboard's alerts list, most-reported first."""
    if not request.session.get("admin_ok"):
        return JSONResponse({"reports": []}, status_code=401)
    return JSONResponse({"reports": db.reports_summary()})


@router.post("/admin/shows/override")
async def admin_shows_override(request: Request, show_key: str = Form(...),
                                source: str = Form(""), title: str = Form(""),
                                air_time: str = Form(""), day_shift: int = Form(0),
                                window_month: str = Form(""), window_year: str = Form(""),
                                air_time_zone: str = Form("")):
    """Save (or clear) one show's correction, then drop its reports as handled."""
    if not request.session.get("admin_ok"):
        return JSONResponse({"ok": False}, status_code=401)

    air_time = (air_time or "").strip()
    if air_time and not re.fullmatch(r"([01]?\d|2[0-3]):[0-5]\d", air_time):
        return JSONResponse({"ok": False, "detail": "Time must be HH:MM (24-hour)"},
                            status_code=400)
    day_shift = max(-1, min(1, int(day_shift)))

    # V5.4.5 — the release window. Year is typed, so it gets checked properly;
    # month is a dropdown and may legitimately be blank, meaning "some time
    # that year".
    wy = (window_year or "").strip()
    wm = (window_month or "").strip()
    year_val = None
    month_val = None
    if wy:
        if not re.fullmatch(r"\d{4}", wy):
            return JSONResponse({"ok": False, "detail": "Year must be four digits"},
                                status_code=400)
        year_val = int(wy)
        if not (1900 <= year_val <= 2999):
            return JSONResponse({"ok": False, "detail": "Year looks wrong"},
                                status_code=400)
        if wm:
            month_val = int(wm)
            if not (1 <= month_val <= 12):
                return JSONResponse({"ok": False, "detail": "Month must be 1-12"},
                                    status_code=400)

    # db.override_set falls back to the default for anything unrecognised, so
    # an old client that posts no zone still produces a valid correction rather
    # than a NULL that would be ambiguous all over again.
    db.override_set(show_key, source, title, air_time or None, day_shift,
                    month_val, year_val, air_time_zone or None)
    db.reports_clear(show_key)   # the reason for the alert has been addressed
    return JSONResponse({"ok": True})


@router.post("/admin/reports/dismiss")
async def admin_reports_dismiss(request: Request, show_key: str = Form(...)):
    """Clear an alert without changing anything — for a report that was wrong."""
    if not request.session.get("admin_ok"):
        return JSONResponse({"ok": False}, status_code=401)
    db.reports_clear(show_key)
    return JSONResponse({"ok": True})


@router.post("/admin/reset-stat")
async def reset_one_stat(request: Request, stat_name: str = Form(...)):
    if not request.session.get("admin_ok"):
        raise HTTPException(status_code=401, detail="Not logged in")
    try:
        db.reset_stat(stat_name)
    except ValueError:
        raise HTTPException(status_code=400, detail=f"'{stat_name}' isn't a resettable stat")
    return {"ok": True}


@router.post("/admin/reset-all-stats")
async def reset_all(request: Request):
    if not request.session.get("admin_ok"):
        raise HTTPException(status_code=401, detail="Not logged in")
    db.reset_all_stats()
    return {"ok": True}
