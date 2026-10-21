"""
Persistent storage for the relay: cache entries, client (installation)
tracking, message-of-the-day, and the human-readable console log.
SQLite specifically because it's built into Python (no extra service to
run), file-based (so "storage usage" is just that one file's size), and
easily fast enough for a personal/small-group relay's request volume.

Note on "storage usage": this only ever holds metadata responses
(JSON) — actual images are never proxied or stored here (see
tmdb_proxy.py/igdb_proxy.py for why), so this database should stay
small — low tens of MB even with thousands of cached titles, not
gigabytes.
"""
import sqlite3
import time
import os
from contextlib import contextmanager

DB_PATH = os.environ.get("RELAY_DB_PATH", "relay.db")

# How many console log lines to keep. This is meant as a live/recent
# view ("what's happening right now"), not a permanent audit trail —
# capping it keeps the table small and the console page fast to load,
# rather than growing forever. Raise this if you want more history.
CONSOLE_LOG_MAX_ENTRIES = 1000


def _connect():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


@contextmanager
def get_conn():
    conn = _connect()
    try:
        yield conn
        conn.commit()
    finally:
        conn.close()


def _migrate_add_feedback_todo_column(conn):
    """
    V5.4.5 — a message can be flagged as something to actually do. Reading a
    bug report and deciding to fix it are different acts, so "read" alone
    couldn't express it: a fix you have planned would either sit unread
    forever or vanish into the read pile.
    """
    cols = [row["name"] for row in conn.execute("PRAGMA table_info(feedback)").fetchall()]
    if "todo" in cols:
        return
    conn.execute("ALTER TABLE feedback ADD COLUMN todo INTEGER NOT NULL DEFAULT 0")


def _migrate_add_override_window_columns(conn):
    """
    V5.4.5 — a correction can now also state WHEN a show returns, not just at
    what time of day. Some shows come back the same rough month every year and
    the person running the relay simply knows that before any source publishes
    it.

    Nullable and added in place, so existing corrections keep working untouched
    and a relay that has never had one needs no special handling.
    """
    cols = [row["name"] for row in conn.execute("PRAGMA table_info(show_overrides)").fetchall()]
    if "window_year" in cols:
        return
    conn.execute("ALTER TABLE show_overrides ADD COLUMN window_month INTEGER")
    conn.execute("ALTER TABLE show_overrides ADD COLUMN window_year INTEGER")


def _migrate_add_override_timezone_column(conn):
    """
    V5.4.28 — an air-time correction had no time zone, which made it
    ambiguous the moment two users were in different ones. "3 AM" entered by
    someone in Eastern was applied verbatim as 3 AM Pacific for a Pacific
    user — three hours out — and nothing recorded what had been meant.

    Existing rows are backfilled to Eastern rather than left NULL, because
    that is what every correction entered so far actually meant: the person
    entering them is in Eastern. Leaving them NULL would mean guessing again
    at read time, which is the problem this column removes.
    """
    cols = [row["name"] for row in conn.execute("PRAGMA table_info(show_overrides)").fetchall()]
    if "air_time_zone" in cols:
        return
    conn.execute("ALTER TABLE show_overrides ADD COLUMN air_time_zone TEXT")
    conn.execute("UPDATE show_overrides SET air_time_zone = ? WHERE air_time_zone IS NULL",
                 (DEFAULT_OVERRIDE_TZ,))


def _migrate_add_friendly_number_column(conn):
    cols = [row["name"] for row in conn.execute("PRAGMA table_info(clients)").fetchall()]
    if "friendly_number" in cols:
        return  # already up to date, nothing to do

    conn.execute("ALTER TABLE clients ADD COLUMN friendly_number INTEGER")

    # Backfill existing installations in the order they were first seen,
    # so whoever's been using the relay longest keeps a low, sensible
    # number rather than everyone ending up blank or in an arbitrary order.
    rows = conn.execute("SELECT client_id FROM clients ORDER BY first_seen ASC").fetchall()
    next_num = 1
    for row in rows:
        conn.execute(
            "UPDATE clients SET friendly_number = ? WHERE client_id = ?",
            (next_num, row["client_id"]),
        )
        next_num += 1

    # Make sure the counter continues on from the backfill, so the next
    # genuinely new installation doesn't collide with one just assigned.
    conn.execute(
        "UPDATE lifetime_stats SET stat_value = ? WHERE stat_name = 'next_friendly_number' AND stat_value < ?",
        (next_num, next_num),
    )


def init_db():
    with get_conn() as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS cache_entries (
                cache_key   TEXT PRIMARY KEY,
                method      TEXT NOT NULL,
                path        TEXT NOT NULL,
                body        BLOB NOT NULL,
                status      INTEGER NOT NULL,
                cached_at   REAL NOT NULL,
                expires_at  REAL NOT NULL,
                hit_count   INTEGER NOT NULL DEFAULT 0
            )
        """)
        conn.execute("CREATE INDEX IF NOT EXISTS idx_expires ON cache_entries(expires_at)")

        conn.execute("""
            CREATE TABLE IF NOT EXISTS clients (
                client_id       TEXT PRIMARY KEY,
                friendly_number INTEGER NOT NULL,
                first_seen      REAL NOT NULL,
                last_seen       REAL NOT NULL,
                request_count   INTEGER NOT NULL DEFAULT 0
            )
        """)

        conn.execute("""
            CREATE TABLE IF NOT EXISTS lifetime_stats (
                stat_name  TEXT PRIMARY KEY,
                stat_value INTEGER NOT NULL DEFAULT 0
            )
        """)
        for name in ("total_requests", "cache_hits", "cache_misses",
                     "unique_fetches_ever", "next_friendly_number",
                     "tiles_refreshed_ever", "tiles_fetched_ever"):
            default = 1 if name == "next_friendly_number" else 0
            conn.execute(
                "INSERT OR IGNORE INTO lifetime_stats (stat_name, stat_value) VALUES (?, ?)",
                (name, default),
            )

        # v0.0.3 added the friendly_number column, but CREATE TABLE IF NOT
        # EXISTS only applies to brand-new databases — a relay.db already
        # running since v0.0.2 has a clients table without it, and every
        # request crashes trying to read a column that was never added.
        # This adds it (only if actually missing) and backfills existing
        # installations with sensible numbers in first-seen order, rather
        # than losing their history or crashing forever.
        _migrate_add_friendly_number_column(conn)

        conn.execute("""
            CREATE TABLE IF NOT EXISTS motd (
                id        INTEGER PRIMARY KEY CHECK (id = 1),
                message   TEXT NOT NULL DEFAULT '',
                updated_at REAL NOT NULL DEFAULT 0
            )
        """)
        conn.execute("INSERT OR IGNORE INTO motd (id, message, updated_at) VALUES (1, '', 0)")

        # V0.0.10 — the "current app version" the admin dashboard publishes,
        # so the desktop app can tell you're running something older and
        # say so. Defaults to 4.4.0 (the version this feature shipped
        # alongside) rather than 0.0.0, so no installation sees a false
        # "you're out of date" the moment this table is first created.
        conn.execute("""
            CREATE TABLE IF NOT EXISTS app_version (
                id      INTEGER PRIMARY KEY CHECK (id = 1),
                major   INTEGER NOT NULL DEFAULT 4,
                minor   INTEGER NOT NULL DEFAULT 4,
                patch   INTEGER NOT NULL DEFAULT 0
            )
        """)
        conn.execute("INSERT OR IGNORE INTO app_version (id, major, minor, patch) VALUES (1, 4, 4, 0)")

        conn.execute("""
            CREATE TABLE IF NOT EXISTS console_log (
                id       INTEGER PRIMARY KEY AUTOINCREMENT,
                ts       REAL NOT NULL,
                message  TEXT NOT NULL
            )
        """)

        # Grows over time as the relay actually sees real titles come
        # back in TMDB/IGDB responses — every detail fetch response
        # includes its own title, and every search result includes one
        # per hit, so this fills in naturally through normal use rather
        # than needing a separate lookup step. Used so the console can
        # show "The Batman" instead of "movie #1091}", once it's been
        # seen at least once.
        conn.execute("""
            CREATE TABLE IF NOT EXISTS known_titles (
                source   TEXT NOT NULL,
                item_id  TEXT NOT NULL,
                title    TEXT NOT NULL,
                PRIMARY KEY (source, item_id)
            )
        """)

        # V5 — per-show air-time corrections, published to every installation.
        #
        # Some services simply aren't covered: TVmaze records no air time for
        # Disney+ at all, so a show that really drops at 9 PM Eastern falls
        # back to the app's generic default. No data source has the real
        # answer, so this is where a human puts it in once for everybody.
        #
        # day_shift handles the related case of a listing being filed under
        # the wrong day — a late-night broadcast is often recorded under the
        # previous evening. -1, 0 or +1 days.
        conn.execute("""
            CREATE TABLE IF NOT EXISTS show_overrides (
                show_key   TEXT PRIMARY KEY,
                source     TEXT NOT NULL,
                title      TEXT NOT NULL DEFAULT '',
                air_time   TEXT,
                day_shift  INTEGER NOT NULL DEFAULT 0,
                updated_at REAL NOT NULL,
                window_month INTEGER,
                window_year  INTEGER,
                air_time_zone TEXT
            )
        """)
        _migrate_add_override_window_columns(conn)
        _migrate_add_override_timezone_column(conn)

        # V5 — "this air time looks wrong" reports from the app.
        #
        # One row per installation per show, so a client can be counted once
        # rather than repeatedly. reported_at is refreshed on a later report,
        # which is what allows a fresh report a day after a correction that
        # turned out to be wrong, without ever letting the count inflate.
        conn.execute("""
            CREATE TABLE IF NOT EXISTS airtime_reports (
                show_key    TEXT NOT NULL,
                client_id   TEXT NOT NULL,
                title       TEXT NOT NULL DEFAULT '',
                source      TEXT NOT NULL DEFAULT '',
                reported_at REAL NOT NULL,
                PRIMARY KEY (show_key, client_id)
            )
        """)
        conn.execute("CREATE INDEX IF NOT EXISTS idx_reports_time ON airtime_reports(reported_at)")

        # V5.4.3 — free-text feedback from inside the app: bug reports and
        # feature ideas.
        #
        # One table with a `kind` column rather than two identical tables,
        # because the only thing that differs between a bug and a request is
        # which inbox it lands in — the storage, the moderation actions and
        # the page that renders them are the same, and splitting them would
        # mean maintaining all of that twice.
        #
        # Unlike airtime_reports this is NOT one row per (client, subject):
        # someone may legitimately report several different bugs, so every
        # submission is its own row. Abuse is bounded by a per-client cooldown
        # and a length cap at the endpoint instead.
        #
        # `read` and nothing else — a mailbox needs "have I dealt with this",
        # not a workflow. Deleting is the other half of that.
        conn.execute("""
            CREATE TABLE IF NOT EXISTS feedback (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                kind        TEXT NOT NULL,
                client_id   TEXT NOT NULL,
                body        TEXT NOT NULL,
                app_version TEXT NOT NULL DEFAULT '',
                created_at  REAL NOT NULL,
                read        INTEGER NOT NULL DEFAULT 0,
                todo        INTEGER NOT NULL DEFAULT 0
            )
        """)
        conn.execute("CREATE INDEX IF NOT EXISTS idx_feedback_kind ON feedback(kind, created_at)")
        _migrate_add_feedback_todo_column(conn)

        # Small key/value store for things the relay itself needs to remember
        # across restarts — currently the secret that signs client ids.
        conn.execute("""
            CREATE TABLE IF NOT EXISTS relay_config (
                key   TEXT PRIMARY KEY,
                value TEXT NOT NULL
            )
        """)
        _migrate_drop_unissued_clients(conn)


# ── Show overrides & air-time reports (V5) ──────────────────────────────────
#
# REPORT_COOLDOWN_SECONDS is why a report is stored per (show, client) rather
# than appended to a log: one installation can only ever contribute one row
# per show, so nobody can inflate a count by clicking repeatedly. Re-reporting
# is still allowed after the cooldown, which matters when a correction was
# itself wrong — the alert simply resurfaces instead of being lost.
REPORT_COOLDOWN_SECONDS = 24 * 3600


# ── What a correction is made of, in one place ──────────────────────────────
#
# V5.4.12. Three separate places hand a correction to the dashboard — the
# The zones a correction may be entered in. Deliberately the same four the
# app offers in Settings, in the same order, so "3 AM Eastern" means one
# thing on both sides. IANA ids, because that is what the app stores.
OVERRIDE_TIME_ZONES = ("America/Los_Angeles", "America/Denver",
                       "America/Chicago", "America/New_York")
DEFAULT_OVERRIDE_TZ = "America/New_York"

# Active list (overrides_all), the alert rows (reports_summary) and a search
# hit (admin.admin_shows_search) — and each used to name the columns itself.
# Every row opens the SAME editor, so a column missing from one of them opens
# blank there and is then SAVED blank: that is exactly how Daredevil: Born
# Again's release window was wiped by someone correcting its air time from an
# alert. Adding a column here now reaches all three at once.
OVERRIDE_COLUMNS = ("air_time", "air_time_zone", "day_shift", "window_month", "window_year")

# What each column looks like when there is no correction at all. The editor
# reads these directly, so "no air time" has to be "" rather than None.
# air_time_zone is never empty: an air time without a zone is ambiguous,
# which is the bug this column exists to fix. Eastern is the default because
# that is what the person entering corrections uses, and because a
# wrong-but-stated zone is fixable while a missing one is not.
OVERRIDE_EMPTY = {"air_time": "", "air_time_zone": DEFAULT_OVERRIDE_TZ,
                  "day_shift": 0, "window_month": None, "window_year": None}


def override_fields(row) -> dict:
    """
    The correction columns for a row that may be None (a show with nothing set
    yet), with the empty defaults the dashboard editor expects.
    """
    if row is None:
        return dict(OVERRIDE_EMPTY)
    return {c: (OVERRIDE_EMPTY[c] if row[c] is None else row[c])
            for c in OVERRIDE_COLUMNS}


def overrides_all() -> list[dict]:
    with get_conn() as conn:
        rows = conn.execute(
            "SELECT show_key, source, title, " + ", ".join(OVERRIDE_COLUMNS) +
            " FROM show_overrides"
        ).fetchall()
        return [dict(r) for r in rows]


def override_set(show_key: str, source: str, title: str,
                 air_time: str | None, day_shift: int,
                 window_month: int | None = None, window_year: int | None = None,
                 air_time_zone: str | None = None):
    """
    Store or clear a correction. A correction with nothing in it is not a
    correction, so an empty time, no shift and no window removes the row —
    that is also how the page's "Remove correction" button works.

    window_year without window_month is a YEAR-wide window ("returns some time
    in 2027"), which is the common case for a show whose return month nobody
    has announced. A month on its own is meaningless — which year? — so it is
    ignored unless a year comes with it.
    """
    if not window_year:
        window_month = None
        window_year = None
    # An air time is meaningless without the zone it was entered in, so one is
    # always stored. Anything unrecognised falls back to the default rather
    # than being kept verbatim -- a zone the app does not know would be silently
    # ignored on the client and we would be back to an ambiguous time.
    if air_time_zone not in OVERRIDE_TIME_ZONES:
        air_time_zone = DEFAULT_OVERRIDE_TZ
    with get_conn() as conn:
        if not air_time and not day_shift and not window_year:
            conn.execute("DELETE FROM show_overrides WHERE show_key = ?", (show_key,))
            return
        conn.execute(
            """
            INSERT INTO show_overrides (show_key, source, title, air_time, day_shift,
                                        updated_at, window_month, window_year,
                                        air_time_zone)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(show_key) DO UPDATE SET
                source = excluded.source, title = excluded.title,
                air_time = excluded.air_time, day_shift = excluded.day_shift,
                window_month = excluded.window_month,
                window_year = excluded.window_year,
                air_time_zone = excluded.air_time_zone,
                updated_at = excluded.updated_at
            """,
            (show_key, source, title, air_time or None, int(day_shift), time.time(),
             window_month, window_year, air_time_zone),
        )


def override_clear_window(show_key: str) -> bool:
    """
    V5.4.6 — drop just the release window from a correction, keeping the air
    time. Returns True when a window was actually there to remove.

    A window is a stand-in for a date nobody has published yet, so it stops
    being true the moment somebody does. It must not survive into the next
    season either: a show that returned in April one year may return in
    September the next, and a window left in place would quietly assert the
    old month against the new season. The air time is the opposite — a slot a
    show airs in doesn't change because a season did — so it is never touched
    here, and a correction that is only an air time survives untouched.
    """
    with get_conn() as conn:
        row = conn.execute(
            "SELECT air_time, day_shift, window_year FROM show_overrides WHERE show_key = ?",
            (show_key,),
        ).fetchone()
        if not row or not row["window_year"]:
            return False
        if row["air_time"] or row["day_shift"]:
            conn.execute(
                "UPDATE show_overrides SET window_month = NULL, window_year = NULL, "
                "updated_at = ? WHERE show_key = ?",
                (time.time(), show_key),
            )
        else:
            # Nothing else was set, so there is no correction left to keep.
            conn.execute("DELETE FROM show_overrides WHERE show_key = ?", (show_key,))
        return True


def report_add(show_key: str, client_id: str, title: str, source: str) -> bool:
    """
    Record one installation's report. Returns False when the same client
    already reported this show inside the cooldown, so the caller can tell a
    genuine report from a repeat without treating the repeat as an error.
    """
    now = time.time()
    with get_conn() as conn:
        row = conn.execute(
            "SELECT reported_at FROM airtime_reports WHERE show_key = ? AND client_id = ?",
            (show_key, client_id),
        ).fetchone()
        if row and (now - row["reported_at"]) < REPORT_COOLDOWN_SECONDS:
            return False
        conn.execute(
            """
            INSERT INTO airtime_reports (show_key, client_id, title, source, reported_at)
            VALUES (?, ?, ?, ?, ?)
            ON CONFLICT(show_key, client_id) DO UPDATE SET
                reported_at = excluded.reported_at, title = excluded.title
            """,
            (show_key, client_id, title, source, now),
        )
        return True


# ── Feedback: bug reports and feature requests (V5.4.3) ─────────────────────
#
# No per-submission cooldown, unlike the air-time report button.
#
# That one is a VOTE — one row per (show, installation), counted, so a repeat
# click has to be refused or the count means nothing. This is a mailbox: every
# message is its own thing, someone may reasonably send several in a row while
# working through a list of problems, and refusing the third one loses
# information that was freely offered. Being told to wait after writing
# something out is also a good way to make someone not bother again.
#
# The relay's ordinary per-installation request limit still applies underneath
# (rate_limit.check at the endpoint), so a stuck button or a script can't
# actually flood it — that guard is about request volume, not about how much
# any one person is allowed to say.
FEEDBACK_MAX_LENGTH = 4000
FEEDBACK_KINDS = ("bug", "request")


def feedback_add(kind: str, client_id: str, body: str, app_version: str) -> bool:
    """
    Store one submission. Always accepted; the bool is kept so the endpoint's
    response shape doesn't change and a future guard has somewhere to live.
    """
    now = time.time()
    with get_conn() as conn:
        conn.execute(
            "INSERT INTO feedback (kind, client_id, body, app_version, created_at, read) "
            "VALUES (?, ?, ?, ?, ?, 0)",
            (kind, client_id, body[:FEEDBACK_MAX_LENGTH], app_version[:40], now),
        )
        return True


def feedback_list(kind: str) -> list[dict]:
    """Newest first — a mailbox reads top-down."""
    with get_conn() as conn:
        rows = conn.execute(
            "SELECT id, kind, client_id, body, app_version, created_at, read, todo "
            "FROM feedback WHERE kind = ? ORDER BY created_at DESC",
            (kind,),
        ).fetchall()
        return [dict(r) for r in rows]


def feedback_unread_counts() -> dict:
    """Per-kind unread totals, for the badges on the nav."""
    with get_conn() as conn:
        rows = conn.execute(
            "SELECT kind, COUNT(*) AS n FROM feedback WHERE read = 0 GROUP BY kind"
        ).fetchall()
        counts = {k: 0 for k in FEEDBACK_KINDS}
        for r in rows:
            counts[r["kind"]] = r["n"]
        return counts


def feedback_set_read(entry_id: int, read: bool):
    with get_conn() as conn:
        conn.execute("UPDATE feedback SET read = ? WHERE id = ?",
                     (1 if read else 0, entry_id))


def feedback_set_todo(entry_id: int, todo: bool):
    """
    Flagging something to do also marks it read — you cannot decide to act on
    a message without having read it, and leaving it bold in the inbox as well
    as listed in the to-do column would just be the same thing twice.
    """
    with get_conn() as conn:
        if todo:
            conn.execute("UPDATE feedback SET todo = 1, read = 1 WHERE id = ?", (entry_id,))
        else:
            conn.execute("UPDATE feedback SET todo = 0 WHERE id = ?", (entry_id,))


def feedback_delete(entry_id: int):
    with get_conn() as conn:
        conn.execute("DELETE FROM feedback WHERE id = ?", (entry_id,))


def reports_summary(limit: int = 50) -> list[dict]:
    """
    Reported shows, most-reported first, with whatever fix already exists.

    Every column of that existing correction has to come back, not just the
    air time. An alert row on the dashboard is a full editor for the show, so
    a field missing here opens blank and is then SAVED blank — which is how a
    release window set on Daredevil: Born Again vanished the moment its air
    time was corrected from the alert.
    """
    with get_conn() as conn:
        rows = conn.execute(
            """
            SELECT r.show_key, r.source,
                   MAX(r.title)        AS title,
                   COUNT(*)            AS reports,
                   MAX(r.reported_at)  AS last_reported,
                   """ + ", ".join(f"o.{c} AS {c}" for c in OVERRIDE_COLUMNS) + """
            FROM airtime_reports r
            LEFT JOIN show_overrides o ON o.show_key = r.show_key
            GROUP BY r.show_key
            ORDER BY reports DESC, last_reported DESC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
        return [dict(r) for r in rows]


def reports_clear(show_key: str):
    with get_conn() as conn:
        conn.execute("DELETE FROM airtime_reports WHERE show_key = ?", (show_key,))


def bump_stat(name: str, by: int = 1):
    with get_conn() as conn:
        conn.execute(
            "UPDATE lifetime_stats SET stat_value = stat_value + ? WHERE stat_name = ?",
            (by, name),
        )
    from app import stats_stream
    stats_stream.broadcast(dashboard_stats())


RESETTABLE_STATS = (
    "total_requests", "cache_hits", "cache_misses",
    "unique_fetches_ever", "tiles_refreshed_ever", "tiles_fetched_ever",
)


def reset_stat(name: str):
    if name not in RESETTABLE_STATS:
        raise ValueError(f"Not a resettable stat: {name}")
    with get_conn() as conn:
        conn.execute("UPDATE lifetime_stats SET stat_value = 0 WHERE stat_name = ?", (name,))
    from app import stats_stream
    stats_stream.broadcast(dashboard_stats())


def reset_all_stats():
    with get_conn() as conn:
        conn.execute(
            f"UPDATE lifetime_stats SET stat_value = 0 WHERE stat_name IN ({','.join('?' for _ in RESETTABLE_STATS)})",
            RESETTABLE_STATS,
        )
        # Installation history — "Installations ever seen" and "Active in
        # last 30 days" — is NEVER reset by this button. Original V0.0.11
        # rule, briefly reversed and now reinstated.
        #
        # It was reversed because the number was wrong: 22 installations when
        # one person had ever run the app, because any X-Client-ID string used
        # while debugging registered as a new one. That was a counting bug,
        # not a reason to make history disposable — ids are issued and signed
        # now (see issue_client_id), so the figure is real and is the one
        # number here you cannot recreate by waiting. Everything else is a
        # tally that starts filling up again immediately.
        #
        # Deliberately NOT touched either: show_overrides, feedback and
        # airtime_reports. Those are things somebody wrote down, not counters
        # — corrections you set, bug reports and requests people sent. A
        # button about numbers must not delete work.
        conn.execute("DELETE FROM cache_entries")
        conn.commit()
        # VACUUM reclaims the disk space SQLite would otherwise just mark
        # as internally reusable without shrinking the file — without
        # this, "Database file size" wouldn't budge even with the table
        # above now empty.
        conn.execute("VACUUM")
    from app import stats_stream
    stats_stream.broadcast(dashboard_stats())


def _migrate_drop_unissued_clients(conn):
    """
    V0.0.13 — one-time clear-out of "installations" that were never real.

    verify_client used to register whatever arrived in X-Client-ID, so every
    ad-hoc value used while debugging with curl became a permanent
    installation: test-1, xmen-debug, dims-test, pick2, halo-dbg, probe and a
    dozen more. That is the whole reason the dashboard read 22 when one person
    had ever run the app.

    Client ids are now issued and signed by the relay (see issue_client_id),
    so nothing unissued can be registered again. This removes the rows that
    predate that rule — including the one genuine installation, whose id is an
    older plain UUID and cannot be verified either. It re-registers with an
    issued id the next time the app runs and lands on User 1, which is what
    the count should have said all along.
    """
    rows = conn.execute("SELECT client_id FROM clients").fetchall()
    stale = [r["client_id"] for r in rows if not verify_client_id(r["client_id"])]
    if not stale:
        return
    conn.executemany("DELETE FROM clients WHERE client_id = ?", [(c,) for c in stale])
    conn.execute(
        "UPDATE lifetime_stats SET stat_value = 1 WHERE stat_name = 'next_friendly_number'")


# ── Relay-issued client ids ─────────────────────────────────────────────────
#
# An id is <32 hex>.<16 hex HMAC of it>, signed with a secret this relay
# generates once and keeps. That makes it verifiable without storing a list of
# valid ids, and makes a hand-written one impossible: someone editing their id
# to a value they made up (or to somebody else's, hoping to merge with them)
# produces something that doesn't verify, so it is refused and replaced with a
# freshly issued one on the next request.
#
# The signing secret lives in its own table rather than being derived from
# RELAY_SHARED_SECRET, so rotating that shared secret — which is a normal
# thing to want to do — doesn't invalidate every id in existence and reset the
# installation count to zero.
CLIENT_ID_RE = None   # compiled lazily below, keeps `re` out of module import


def _client_id_secret(conn) -> str:
    row = conn.execute(
        "SELECT value FROM relay_config WHERE key = 'client_id_secret'").fetchone()
    if row:
        return row["value"]
    import secrets
    secret = secrets.token_hex(32)
    conn.execute("INSERT INTO relay_config (key, value) VALUES ('client_id_secret', ?)",
                 (secret,))
    return secret


def _sign_client_id(raw: str, secret: str) -> str:
    import hmac, hashlib
    return hmac.new(secret.encode(), raw.encode(), hashlib.sha256).hexdigest()[:16]


def issue_client_id() -> str:
    import secrets
    with get_conn() as conn:
        secret = _client_id_secret(conn)
        raw = secrets.token_hex(16)
        return f"{raw}.{_sign_client_id(raw, secret)}"


def verify_client_id(client_id: str) -> bool:
    """True only for an id this relay issued. Cheap, and does not touch the
    clients table — it is asked on every single request."""
    import re as _re, hmac as _hmac
    global CLIENT_ID_RE
    if CLIENT_ID_RE is None:
        CLIENT_ID_RE = _re.compile(r"^[0-9a-f]{32}\.[0-9a-f]{16}$")
    if not client_id or not CLIENT_ID_RE.match(client_id):
        return False
    raw, sig = client_id.split(".")
    with get_conn() as conn:
        secret = _client_id_secret(conn)
    return _hmac.compare_digest(sig, _sign_client_id(raw, secret))


def touch_client(client_id: str) -> int:
    """Records this client's activity and returns their friendly number
    (e.g. 365 for "User 365") — assigned once, the first time a given
    client_id is ever seen, then stable forever after."""
    now = time.time()
    with get_conn() as conn:
        existing = conn.execute(
            "SELECT friendly_number FROM clients WHERE client_id = ?", (client_id,)
        ).fetchone()
        if existing is not None:
            conn.execute(
                "UPDATE clients SET last_seen = ?, request_count = request_count + 1 WHERE client_id = ?",
                (now, client_id),
            )
            return existing["friendly_number"]

        # Brand new installation — hand out the next friendly number and
        # advance the counter so the one after gets a different one.
        row = conn.execute(
            "SELECT stat_value FROM lifetime_stats WHERE stat_name = 'next_friendly_number'"
        ).fetchone()
        friendly_number = row["stat_value"]
        conn.execute(
            "UPDATE lifetime_stats SET stat_value = stat_value + 1 WHERE stat_name = 'next_friendly_number'"
        )
        conn.execute(
            """
            INSERT INTO clients (client_id, friendly_number, first_seen, last_seen, request_count)
            VALUES (?, ?, ?, ?, 1)
            """,
            (client_id, friendly_number, now, now),
        )
        return friendly_number


def add_console_log(message: str):
    now = time.time()
    with get_conn() as conn:
        conn.execute("INSERT INTO console_log (ts, message) VALUES (?, ?)", (now, message))
        # Trim old entries so this stays a bounded, "recent activity"
        # view rather than growing forever.
        conn.execute(
            """
            DELETE FROM console_log WHERE id NOT IN (
                SELECT id FROM console_log ORDER BY id DESC LIMIT ?
            )
            """,
            (CONSOLE_LOG_MAX_ENTRIES,),
        )


def get_recent_console_log(limit: int = 200):
    with get_conn() as conn:
        rows = conn.execute(
            "SELECT ts, message FROM console_log ORDER BY id DESC LIMIT ?", (limit,)
        ).fetchall()
    return [{"ts": r["ts"], "message": r["message"]} for r in rows]


def cache_get(key: str):
    with get_conn() as conn:
        row = conn.execute(
            "SELECT body, status, expires_at FROM cache_entries WHERE cache_key = ?", (key,)
        ).fetchone()
        if row is None:
            return None
        if row["expires_at"] < time.time():
            conn.execute("DELETE FROM cache_entries WHERE cache_key = ?", (key,))
            return None
        conn.execute("UPDATE cache_entries SET hit_count = hit_count + 1 WHERE cache_key = ?", (key,))
        return {"body": row["body"], "status": row["status"]}


def cache_set(key: str, method: str, path: str, body: bytes, status: int, ttl_seconds: int):
    now = time.time()
    with get_conn() as conn:
        conn.execute(
            """
            INSERT INTO cache_entries (cache_key, method, path, body, status, cached_at, expires_at, hit_count)
            VALUES (?, ?, ?, ?, ?, ?, ?, 0)
            ON CONFLICT(cache_key) DO UPDATE SET
                body = excluded.body, status = excluded.status,
                cached_at = excluded.cached_at, expires_at = excluded.expires_at
            """,
            (key, method, path, body, status, now, now + ttl_seconds),
        )


def remember_title(source: str, item_id: str, title: str):
    if not title:
        return
    with get_conn() as conn:
        conn.execute(
            """
            INSERT INTO known_titles (source, item_id, title) VALUES (?, ?, ?)
            ON CONFLICT(source, item_id) DO UPDATE SET title = excluded.title
            """,
            (source, str(item_id), title),
        )


def lookup_title(source: str, item_id: str) -> str | None:
    with get_conn() as conn:
        row = conn.execute(
            "SELECT title FROM known_titles WHERE source = ? AND item_id = ?",
            (source, str(item_id)),
        ).fetchone()
    return row["title"] if row else None


def get_motd() -> str:
    with get_conn() as conn:
        row = conn.execute("SELECT message FROM motd WHERE id = 1").fetchone()
        return row["message"] if row else ""


def set_motd(message: str):
    with get_conn() as conn:
        conn.execute(
            "UPDATE motd SET message = ?, updated_at = ? WHERE id = 1", (message, time.time())
        )


def get_app_version() -> tuple[int, int, int]:
    with get_conn() as conn:
        row = conn.execute("SELECT major, minor, patch FROM app_version WHERE id = 1").fetchone()
        return (row["major"], row["minor"], row["patch"]) if row else (0, 0, 0)


def set_app_version(major: int, minor: int, patch: int):
    with get_conn() as conn:
        conn.execute(
            "UPDATE app_version SET major = ?, minor = ?, patch = ? WHERE id = 1",
            (major, minor, patch),
        )


def dashboard_stats() -> dict:
    with get_conn() as conn:
        stats_rows = conn.execute("SELECT stat_name, stat_value FROM lifetime_stats").fetchall()
        stats = {r["stat_name"]: r["stat_value"] for r in stats_rows}

        cache_row = conn.execute(
            "SELECT COUNT(*) AS n, COALESCE(SUM(LENGTH(body)), 0) AS bytes FROM cache_entries WHERE expires_at > ?",
            (time.time(),),
        ).fetchone()

        active_cutoff = time.time() - 30 * 86400  # "active" = seen in the last 30 days
        clients_row = conn.execute(
            "SELECT COUNT(*) AS total, SUM(CASE WHEN last_seen > ? THEN 1 ELSE 0 END) AS active FROM clients",
            (active_cutoff,),
        ).fetchone()

    db_size_bytes = os.path.getsize(DB_PATH) if os.path.exists(DB_PATH) else 0

    return {
        "total_requests": stats.get("total_requests", 0),
        "cache_hits": stats.get("cache_hits", 0),
        "cache_misses": stats.get("cache_misses", 0),
        "unique_fetches_ever": stats.get("unique_fetches_ever", 0),
        "tiles_refreshed_ever": stats.get("tiles_refreshed_ever", 0),
        "tiles_fetched_ever": stats.get("tiles_fetched_ever", 0),
        "tiles_currently_cached": cache_row["n"],
        "cache_bytes": cache_row["bytes"],
        "db_file_bytes": db_size_bytes,
        "total_installations_ever": clients_row["total"] or 0,
        "active_installations_30d": clients_row["active"] or 0,
    }
