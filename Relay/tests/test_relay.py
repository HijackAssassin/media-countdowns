"""
Tests for the relay's OWN logic — auth, routing, whitelisting, caching,
rate limiting, client tracking, and the admin dashboard/MOTD. These run
with MOCK_UPSTREAM=true, so no real TMDB/IGDB credentials are needed or
contacted; the goal here is proving the relay's plumbing works
correctly, not that TMDB/IGDB themselves work.
"""
import os
os.environ["MOCK_UPSTREAM"] = "true"
os.environ["RELAY_SHARED_SECRET"] = "test-secret-123"
os.environ["ADMIN_PASSWORD"] = "test-admin-pw"
os.environ["RATE_LIMIT_PER_MINUTE"] = "3"
os.environ["RELAY_DB_PATH"] = "test_relay.db"

import pytest
import asyncio
from httpx import AsyncClient, ASGITransport
from app.main import app
from app import db, rate_limit

# Client ids must now be issued and signed by the relay, so tests can no
# longer make one up — an invented id is served normally but never counts as
# an installation, which is the whole point of the change.
AUTH_HEADER = {"Authorization": "Bearer test-secret-123", "X-Client-ID": ""}


def issued_header():
    """A header carrying a freshly issued, verifiable client id."""
    return {"Authorization": "Bearer test-secret-123", "X-Client-ID": db.issue_client_id()}


@pytest.fixture(autouse=True)
def _reset_state():
    if os.path.exists("test_relay.db"):
        os.remove("test_relay.db")
    db.init_db()
    AUTH_HEADER["X-Client-ID"] = db.issue_client_id()
    rate_limit._windows.clear()
    from app import refresh_batch
    for entry in refresh_batch._pending.values():
        if entry["task"] is not None:
            entry["task"].cancel()
    refresh_batch._pending.clear()
    yield


async def _client():
    transport = ASGITransport(app=app)
    return AsyncClient(transport=transport, base_url="http://test")


@pytest.mark.asyncio
async def test_health_needs_no_auth():
    async with await _client() as c:
        r = await c.get("/health")
        assert r.status_code == 200
        assert r.json()["status"] == "ok"


@pytest.mark.asyncio
async def test_tmdb_route_rejects_missing_auth():
    async with await _client() as c:
        r = await c.get("/tmdb/search/multi", params={"query": "Inception"})
        assert r.status_code == 401


@pytest.mark.asyncio
async def test_tmdb_route_rejects_wrong_token():
    async with await _client() as c:
        r = await c.get(
            "/tmdb/search/multi",
            params={"query": "Inception"},
            headers={"Authorization": "Bearer wrong-token", "X-Client-ID": "x"},
        )
        assert r.status_code == 401


@pytest.mark.asyncio
async def test_tmdb_route_succeeds_with_correct_token():
    async with await _client() as c:
        r = await c.get(
            "/tmdb/search/multi", params={"query": "Inception"}, headers=AUTH_HEADER
        )
        assert r.status_code == 200
        assert "results" in r.json()


@pytest.mark.asyncio
async def test_tmdb_route_rejects_non_whitelisted_path():
    async with await _client() as c:
        r = await c.get("/tmdb/account", headers=AUTH_HEADER)
        assert r.status_code == 404


@pytest.mark.asyncio
async def test_tmdb_whitelist_matches_the_real_client_exactly():
    # Regression test for a real bug: the whitelist previously allowed
    # "search/movie"/"search/tv" (a guess made before the actual client
    # code had been read closely) but not "search/multi" — the endpoint
    # tmdbscraper.cpp actually calls for every search. Every real search
    # 404'd while other calls worked, exactly matching what was reported.
    async with await _client() as c:
        r_real = await c.get("/tmdb/search/multi", params={"query": "Inception"}, headers=AUTH_HEADER)
        assert r_real.status_code == 200, "search/multi (what the client actually calls) must be allowed"

        r_unused = await c.get("/tmdb/search/movie", params={"query": "Inception"}, headers=AUTH_HEADER)
        assert r_unused.status_code == 404, "search/movie is never actually used by the client — should stay unlisted"


@pytest.mark.asyncio
async def test_igdb_route_succeeds_with_correct_token():
    async with await _client() as c:
        r = await c.post(
            "/igdb/games", content=b'search "Halo"; fields name;', headers=AUTH_HEADER
        )
        assert r.status_code == 200
        assert r.json()[0]["name"] == "Mock Game"


@pytest.mark.asyncio
async def test_igdb_route_rejects_non_whitelisted_path():
    async with await _client() as c:
        r = await c.post("/igdb/some_other_endpoint", content=b"", headers=AUTH_HEADER)
        assert r.status_code == 404


@pytest.mark.asyncio
async def test_caching_avoids_a_second_upstream_call(monkeypatch):
    call_count = {"n": 0}
    from app import tmdb_proxy

    real_forward = tmdb_proxy.forward

    async def counting_forward(path, query, client):
        call_count["n"] += 1
        return await real_forward(path, query, client)

    monkeypatch.setattr(tmdb_proxy, "forward", counting_forward)

    async with await _client() as c:
        r1 = await c.get("/tmdb/search/multi", params={"query": "Dune"}, headers=AUTH_HEADER)
        r2 = await c.get("/tmdb/search/multi", params={"query": "Dune"}, headers=AUTH_HEADER)
        assert r1.status_code == 200 and r2.status_code == 200
        assert r1.content == r2.content

    assert call_count["n"] == 1, "second identical request should have been served from cache"


@pytest.mark.asyncio
async def test_cache_survives_across_requests_persistently():
    # Unlike the old in-memory cache, this should be backed by the
    # SQLite file — proving it isn't just an in-process dict by
    # checking the data is actually queryable via app.db directly.
    async with await _client() as c:
        await c.get("/tmdb/search/multi", params={"query": "Persistent Check"}, headers=AUTH_HEADER)
    stats = db.dashboard_stats()
    assert stats["tiles_currently_cached"] >= 1


@pytest.mark.asyncio
async def test_rate_limit_kicks_in_per_installation():
    async with await _client() as c:
        for i in range(3):
            r = await c.get("/tmdb/search/multi", params={"query": f"q{i}"}, headers=AUTH_HEADER)
            assert r.status_code == 200
        r_over = await c.get("/tmdb/search/multi", params={"query": "one-too-many"}, headers=AUTH_HEADER)
        assert r_over.status_code == 429


@pytest.mark.asyncio
async def test_rate_limit_is_independent_per_installation():
    # A DIFFERENT installation (different X-Client-ID) should have its
    # own, unaffected rate budget, even using the same valid secret.
    other_header = issued_header()
    async with await _client() as c:
        for i in range(3):
            r = await c.get("/tmdb/search/multi", params={"query": f"q{i}"}, headers=AUTH_HEADER)
            assert r.status_code == 200
        r = await c.get("/tmdb/search/multi", params={"query": "other"}, headers=other_header)
        assert r.status_code == 200, "a different installation should have its own budget"


@pytest.mark.asyncio
async def test_client_tracking_counts_distinct_installations():
    header_a = issued_header()
    header_b = issued_header()
    async with await _client() as c:
        await c.get("/tmdb/search/multi", params={"query": "x"}, headers=header_a)
        await c.get("/tmdb/search/multi", params={"query": "y"}, headers=header_a)  # same install again
        await c.get("/tmdb/search/multi", params={"query": "z"}, headers=header_b)

    stats = db.dashboard_stats()
    assert stats["total_installations_ever"] == 2, "two distinct client IDs should count as two installations"


@pytest.mark.asyncio
async def test_admin_dashboard_requires_login():
    async with await _client() as c:
        r = await c.get("/admin", follow_redirects=False)
        assert r.status_code in (302, 303, 307)


@pytest.mark.asyncio
async def test_admin_login_wrong_password_rejected():
    async with await _client() as c:
        r = await c.post("/admin/login", data={"password": "wrong"})
        assert r.status_code == 401


@pytest.mark.asyncio
async def test_admin_login_correct_password_then_dashboard_visible():
    async with await _client() as c:
        r = await c.post("/admin/login", data={"password": "test-admin-pw"}, follow_redirects=True)
        assert r.status_code == 200
        assert "Media Countdowns Relay" in r.text


@pytest.mark.asyncio
async def test_dashboard_renders_current_version_boxes_correctly():
    async with await _client() as c:
        await c.post("/admin/login", data={"password": "test-admin-pw"})
        await c.post("/admin/version", data={"major": 7, "minor": 1, "patch": 22})

        r = await c.get("/admin")
        assert r.status_code == 200
        assert 'value="7"' in r.text
        assert 'value="1"' in r.text
        assert 'value="22"' in r.text


@pytest.mark.asyncio
async def test_dashboard_cache_label_is_clarified_and_titles_are_bold():
    async with await _client() as c:
        await c.post("/admin/login", data={"password": "test-admin-pw"})
        r = await c.get("/admin")
        assert r.status_code == 200
        assert "Entries currently cached" not in r.text, "the old, confusing label should be gone"
        assert "Cached API responses" in r.text
        assert "Not the same as the number of tiles" in r.text, "should directly answer the tiles-or-what confusion"
        assert "font-weight:700" in r.text, "card titles should render bold"


@pytest.mark.asyncio
async def test_motd_public_endpoint_reflects_admin_update():
    async with await _client() as c:
        # No MOTD set yet — should be empty.
        r0 = await c.get("/motd")
        assert r0.json()["message"] == ""

        # Log in and set one.
        await c.post("/admin/login", data={"password": "test-admin-pw"})
        await c.post("/admin/motd", data={"message": "Scheduled maintenance Friday"})

        # Public endpoint (no auth) should now reflect it — this is what
        # any app installation would read, once the client side is
        # updated to check it.
        r1 = await c.get("/motd")
        assert r1.json()["message"] == "Scheduled maintenance Friday"


@pytest.mark.asyncio
async def test_motd_setting_requires_login():
    async with await _client() as c:
        r = await c.post("/admin/motd", data={"message": "should not be allowed"}, follow_redirects=False)
        assert r.status_code in (302, 303, 307)
        r2 = await c.get("/motd")
        assert r2.json()["message"] == ""


@pytest.mark.asyncio
async def test_version_public_endpoint_defaults_sensibly():
    async with await _client() as c:
        # Fresh DB — should default to 4.4.0 (the version this feature
        # shipped alongside), never 0.0.0, so no installation sees a
        # false "out of date" the moment this table is first created.
        r = await c.get("/version")
        assert r.json()["version"] == "4.4.0"


@pytest.mark.asyncio
async def test_version_public_endpoint_reflects_admin_update():
    async with await _client() as c:
        await c.post("/admin/login", data={"password": "test-admin-pw"})
        await c.post("/admin/version", data={"major": 5, "minor": 2, "patch": 13})

        r = await c.get("/version")
        assert r.json()["version"] == "5.2.13"


@pytest.mark.asyncio
async def test_version_setting_requires_login():
    async with await _client() as c:
        r = await c.post("/admin/version", data={"major": 9, "minor": 9, "patch": 9}, follow_redirects=False)
        assert r.status_code in (302, 303, 307)
        r2 = await c.get("/version")
        assert r2.json()["version"] == "4.4.0"   # unchanged


@pytest.mark.asyncio
async def test_version_setting_clamps_out_of_range_values():
    async with await _client() as c:
        await c.post("/admin/login", data={"password": "test-admin-pw"})
        await c.post("/admin/version", data={"major": -5, "minor": 999999, "patch": 3})

        r = await c.get("/version")
        parts = r.json()["version"].split(".")
        assert int(parts[0]) == 0       # negative clamped up to 0
        assert int(parts[1]) == 9999    # huge number clamped down to the max
        assert int(parts[2]) == 3


def test_describe_tmdb_search():
    from app.describe import describe_tmdb_request
    desc = describe_tmdb_request("search/multi", {"query": ["Minecraft"]})
    assert desc == 'search results for "Minecraft"'


def test_describe_tmdb_movie_details():
    from app.describe import describe_tmdb_request
    desc = describe_tmdb_request("movie/27205", {})
    assert desc == "details for movie #27205"


def test_describe_tmdb_show_season():
    from app.describe import describe_tmdb_request
    desc = describe_tmdb_request("tv/1416/season/3", {})
    assert desc == "season 3 details for show #1416"


def test_describe_tmdb_credits():
    from app.describe import describe_tmdb_request
    desc = describe_tmdb_request("tv/1416/credits", {})
    assert desc == "credits for show #1416"


def test_describe_igdb_search():
    from app.describe import describe_igdb_request
    desc = describe_igdb_request(b'search "Halo"; fields name;')
    assert desc == 'search results for "Halo"'


def test_describe_igdb_details():
    from app.describe import describe_igdb_request
    desc = describe_igdb_request(b"fields name; where id = 991;")
    assert desc == "details for game #991"


@pytest.mark.asyncio
async def test_friendly_user_numbers_assigned_in_order_and_stay_stable():
    header_first = issued_header()
    header_second = issued_header()
    async with await _client() as c:
        await c.get("/tmdb/search/multi", params={"query": "a"}, headers=header_first)
        await c.get("/tmdb/search/multi", params={"query": "b"}, headers=header_second)
        # First installation again — should reuse its original number, not get a new one.
        await c.get("/tmdb/search/multi", params={"query": "c"}, headers=header_first)

    log = db.get_recent_console_log(limit=50)
    messages = [e["message"] for e in log]
    assert any("(User 1) requested" in m for m in messages), "first installation should be User 1"
    assert any("(User 2) requested" in m for m in messages), "second installation should be User 2"
    # Confirm installation-first's third request still says "User 1", not "User 3".
    third_request_msgs = [m for m in messages if '"a"' in m or '"c"' in m]
    assert all("User 1" in m for m in third_request_msgs), "the same installation should keep the same number across requests"


@pytest.mark.asyncio
async def test_console_log_records_human_readable_request_and_sent_lines():
    async with await _client() as c:
        await c.get("/tmdb/search/multi", params={"query": "Dune Part Two"}, headers=AUTH_HEADER)

    log = db.get_recent_console_log(limit=20)
    messages = [e["message"] for e in log]
    assert any('requested search results for "Dune Part Two"' in m for m in messages)
    assert any('Sent (User' in m and 'search results for "Dune Part Two"' in m for m in messages)


@pytest.mark.asyncio
async def test_admin_console_requires_login():
    async with await _client() as c:
        r = await c.get("/admin/console", follow_redirects=False)
        assert r.status_code in (302, 303, 307)


@pytest.mark.asyncio
async def test_admin_console_shows_logged_entries_once_logged_in():
    async with await _client() as c:
        await c.get("/tmdb/search/multi", params={"query": "Console Visibility Check"}, headers=AUTH_HEADER)
        await c.post("/admin/login", data={"password": "test-admin-pw"})
        r = await c.get("/admin/console")
        assert r.status_code == 200
        assert "Console Visibility Check" in r.text


def test_search_requests_get_the_longer_ttl():
    from app.describe import is_tmdb_search_request, is_igdb_search_request
    assert is_tmdb_search_request("search/multi") is True
    assert is_tmdb_search_request("movie/27205") is False
    assert is_igdb_search_request(b'search "Halo"; fields name;') is True
    assert is_igdb_search_request(b"fields name; where id = 991;") is False


@pytest.mark.asyncio
async def test_search_and_details_get_different_cache_ttls():
    async with await _client() as c:
        await c.get("/tmdb/search/multi", params={"query": "TTL Check"}, headers=AUTH_HEADER)
        await c.get("/tmdb/movie/999999", headers=AUTH_HEADER)

    with db.get_conn() as conn:
        rows = conn.execute("SELECT path, cached_at, expires_at FROM cache_entries").fetchall()
    by_path = {r["path"]: r for r in rows}

    search_row = by_path["tmdb/search/multi"]
    details_row = by_path["tmdb/movie/999999"]
    search_ttl = search_row["expires_at"] - search_row["cached_at"]
    details_ttl = details_row["expires_at"] - details_row["cached_at"]

    assert search_ttl > details_ttl, "search results should be cached longer than detail/date fetches"
    assert abs(search_ttl - 86400) < 5, "search TTL should be the requested 24 hours"
    assert abs(details_ttl - 21600) < 5, "details TTL should be the requested 6 hours"


@pytest.mark.asyncio
async def test_console_stream_requires_login():
    async with await _client() as c:
        r = await c.get("/admin/console/stream")
        assert r.status_code == 401


@pytest.mark.asyncio
async def test_console_stream_delivers_a_live_broadcast_message():
    from app import live_log
    async with await _client() as c:
        await c.post("/admin/login", data={"password": "test-admin-pw"})

        queue = live_log.subscribe()
        try:
            live_log.broadcast("User 7 requested search results for \"Test Stream\"")
            message = await asyncio.wait_for(queue.get(), timeout=1.0)
            assert message == "User 7 requested search results for \"Test Stream\""
        finally:
            live_log.unsubscribe(queue)


def test_migration_adds_missing_friendly_number_column_to_an_old_database():
    # Regression test for a real bug: a relay.db that had been running
    # since before the friendly_number column existed crashed on every
    # single request with "no such column: friendly_number", since
    # CREATE TABLE IF NOT EXISTS only applies to brand-new databases, not
    # ones already on disk with the old shape. Simulates exactly that old
    # shape, then confirms init_db() migrates it cleanly and in-place.
    import sqlite3
    import os as os_module

    old_db_path = "test_migration.db"
    if os_module.path.exists(old_db_path):
        os_module.remove(old_db_path)

    conn = sqlite3.connect(old_db_path)
    conn.execute("""
        CREATE TABLE clients (
            client_id TEXT PRIMARY KEY, first_seen REAL NOT NULL,
            last_seen REAL NOT NULL, request_count INTEGER NOT NULL DEFAULT 0
        )
    """)
    conn.execute("CREATE TABLE lifetime_stats (stat_name TEXT PRIMARY KEY, stat_value INTEGER NOT NULL DEFAULT 0)")
    for name in ("total_requests", "cache_hits", "cache_misses", "unique_fetches_ever"):
        conn.execute("INSERT INTO lifetime_stats (stat_name, stat_value) VALUES (?, 0)", (name,))
    conn.execute("INSERT INTO clients VALUES ('old-installation', 1000.0, 5000.0, 42)")
    conn.commit()
    conn.close()

    original_path = db.DB_PATH
    db.DB_PATH = old_db_path
    try:
        db.init_db()  # this must not raise

        with db.get_conn() as conn:
            cols = [row["name"] for row in conn.execute("PRAGMA table_info(clients)").fetchall()]
            assert "friendly_number" in cols

            # The backfill runs first and gives it User 1...
            # ...and then the V0.0.13 clear-out removes it again, because
            # 'old-installation' is not an id this relay ever issued. Both
            # steps are correct: the column migration must not crash on an old
            # database, and an unissued id must not survive as an installation.
            row = conn.execute(
                "SELECT friendly_number FROM clients WHERE client_id = 'old-installation'"
            ).fetchone()
            assert row is None, "an id the relay never issued is not an installation"

        # A real request against this migrated database must not crash, and
        # numbering starts from 1 because nothing real was left behind.
        friendly = db.touch_client(db.issue_client_id())
        assert friendly == 1
    finally:
        db.DB_PATH = original_path
        if os_module.path.exists(old_db_path):
            os_module.remove(old_db_path)


def test_titles_learned_from_tmdb_detail_response_are_then_used_in_descriptions():
    from app import titles
    from app.describe import describe_tmdb_request

    body = b'{"id": 1091, "title": "The Batman", "release_date": "2022-03-04"}'
    titles.remember_from_tmdb_response("movie/1091", body)

    desc = describe_tmdb_request("movie/1091", {})
    assert desc == 'details for "The Batman"'


def test_titles_learned_from_tmdb_search_are_then_used_in_descriptions():
    from app import titles
    from app.describe import describe_tmdb_request

    body = (
        b'{"results": ['
        b'{"id": 200875, "media_type": "tv", "name": "Some Show"},'
        b'{"id": 27205, "media_type": "movie", "title": "Inception"}'
        b']}'
    )
    titles.remember_from_tmdb_response("search/multi", body)

    assert describe_tmdb_request("tv/200875", {}) == 'details for "Some Show"'
    assert describe_tmdb_request("tv/200875/season/1", {}) == 'season 1 details for "Some Show"'
    assert describe_tmdb_request("movie/27205/credits", {}) == 'credits for "Inception"'


def test_season_response_does_not_incorrectly_overwrite_the_shows_own_name():
    from app import titles
    from app.describe import describe_tmdb_request

    # First, the show's real name is learned from a plain detail fetch.
    titles.remember_from_tmdb_response("tv/200875", b'{"id": 200875, "name": "The Real Show"}')
    # Then a season response comes in — its own "name" field is the
    # SEASON's name ("Season 1"), not the show's, and must not clobber it.
    titles.remember_from_tmdb_response("tv/200875/season/1", b'{"id": 55555, "name": "Season 1"}')

    assert describe_tmdb_request("tv/200875", {}) == 'details for "The Real Show"'


def test_igdb_title_learned_from_search_response_is_used_in_later_details_description():
    from app import titles
    from app.describe import describe_igdb_request

    titles.remember_from_igdb_response(b'[{"id": 991, "name": "Halo 4"}]')
    desc = describe_igdb_request(b"fields name; where id = 991;")
    assert desc == 'details for "Halo 4"'


def test_unknown_id_falls_back_to_bare_number():
    from app.describe import describe_tmdb_request, describe_igdb_request
    assert describe_tmdb_request("movie/9999999999", {}) == "details for movie #9999999999"
    assert describe_igdb_request(b"fields name; where id = 9999999999;") == "details for game #9999999999"


@pytest.mark.asyncio
async def test_bulk_detail_refresh_gets_batched_into_one_summary_line_not_many():
    from app import refresh_batch
    original_delay = refresh_batch.FLUSH_DELAY_SECONDS
    refresh_batch.FLUSH_DELAY_SECONDS = 0.05
    try:
        header = issued_header()
        async with await _client() as c:
            # Simulates a startup refresh: several different tiles'
            # details fetched in quick succession by the same user.
            await c.get("/tmdb/movie/111", headers=header)
            await c.get("/tmdb/tv/222", headers=header)
            await c.get("/tmdb/tv/222/season/1", headers=header)  # same tile as tv/222 above
            await asyncio.sleep(0.15)  # let the debounce settle and flush

        log = db.get_recent_console_log(limit=50)
        messages = [e["message"] for e in log]

        # Should NOT see individual "Sent ... details for movie #111" lines
        # for these — they should only appear inside one combined summary.
        assert not any("Sent" in m and "details for movie #111" in m and "Refreshed" not in m for m in messages)

        summary_lines = [m for m in messages if "Refreshed" in m]
        assert len(summary_lines) == 1, f"expected exactly one combined summary line, got {summary_lines}"
        assert "Refreshed 2 tiles" in summary_lines[0], (
            "movie #111 and tv #222 are 2 distinct tiles — the season call for "
            "tv #222 must merge into the same tile, not count as a 3rd"
        )
    finally:
        refresh_batch.FLUSH_DELAY_SECONDS = original_delay


@pytest.mark.asyncio
async def test_searches_are_never_batched_stay_individual_and_immediate():
    from app import refresh_batch
    original_delay = refresh_batch.FLUSH_DELAY_SECONDS
    refresh_batch.FLUSH_DELAY_SECONDS = 0.05
    try:
        header = issued_header()
        async with await _client() as c:
            await c.get("/tmdb/search/multi", params={"query": "Immediate Search Check"}, headers=header)

        # No sleep here at all — a search must show up right away,
        # unlike detail requests which wait for the debounce window.
        log = db.get_recent_console_log(limit=20)
        messages = [e["message"] for e in log]
        assert any('Sent (User' in m and 'Immediate Search Check' in m for m in messages)
    finally:
        refresh_batch.FLUSH_DELAY_SECONDS = original_delay


@pytest.mark.asyncio
async def test_different_users_refresh_batches_do_not_get_merged_together():
    from app import refresh_batch
    original_delay = refresh_batch.FLUSH_DELAY_SECONDS
    refresh_batch.FLUSH_DELAY_SECONDS = 0.05
    try:
        header_a = issued_header()
        header_b = issued_header()
        async with await _client() as c:
            await c.get("/tmdb/movie/301", headers=header_a)
            await c.get("/tmdb/movie/302", headers=header_b)
            await asyncio.sleep(0.15)

        log = db.get_recent_console_log(limit=50)
        summary_lines = [e["message"] for e in log if "Refreshed" in e["message"]]
        assert len(summary_lines) == 2, f"expected two separate per-user summaries, got {summary_lines}"
        assert any("Refreshed 1 tile (1 Movie)" in m for m in summary_lines)
    finally:
        refresh_batch.FLUSH_DELAY_SECONDS = original_delay


@pytest.mark.asyncio
async def test_refresh_batch_marks_failed_items_separately():
    from app import refresh_batch
    original_delay = refresh_batch.FLUSH_DELAY_SECONDS
    refresh_batch.FLUSH_DELAY_SECONDS = 0.05
    try:
        # tv/{id}/season/{n} isn't in the mock-upstream's handled shapes
        # for a non-200 response naturally — use a whitelisted-but-odd
        # path shape indirectly isn't simple here, so this test relies
        # on the mock always returning 200; a genuine failure path is
        # already covered at the unit level by refresh_batch's own
        # _format_summary logic below instead.
        from app.refresh_batch import _format_summary
        items = {
            ("movie", "1"): {"name": "Good Movie", "ok": True, "from_cache": False},
            ("show", "2"): {"name": None, "ok": False, "from_cache": False},
        }
        msg = _format_summary("User 9", "refresh", items)
        assert msg.startswith("(User 9) Refreshed 2 tiles (1 Movie)")
        assert "1 failed: show #2" in msg
        assert "Good Movie" not in msg, "successful items are now grouped by count, not listed by name"
    finally:
        refresh_batch.FLUSH_DELAY_SECONDS = original_delay


def test_tiles_refreshed_and_fetched_stats_are_tracked_separately():
    # tiles_refreshed_ever counts detail/season/credits requests;
    # tiles_fetched_ever counts search requests — verified as distinct,
    # independently-incrementing counters.
    db.bump_stat("tiles_refreshed_ever", 3)
    db.bump_stat("tiles_fetched_ever", 5)
    stats = db.dashboard_stats()
    assert stats["tiles_refreshed_ever"] >= 3
    assert stats["tiles_fetched_ever"] >= 5


def test_refresh_summary_groups_successful_tiles_by_type_count():
    from app.refresh_batch import _format_summary
    items = {
        ("movie", "1"): {"name": "Movie A", "ok": True, "from_cache": False},
        ("movie", "2"): {"name": "Movie B", "ok": True, "from_cache": False},
        ("show", "3"): {"name": None, "ok": True, "from_cache": False},
        ("game", "4"): {"name": None, "ok": True, "from_cache": False},
    }
    msg = _format_summary("User 5", "refresh", items)
    assert msg == "(User 5) Refreshed 4 tiles (2 Movies, 1 Show, 1 Game)"


def test_refresh_summary_notes_when_everything_came_from_cache():
    from app.refresh_batch import _format_summary
    all_cached = {
        ("movie", "1"): {"name": None, "ok": True, "from_cache": True},
        ("show", "2"): {"name": None, "ok": True, "from_cache": True},
    }
    msg = _format_summary("User 5", "refresh", all_cached)
    assert "(all from cache)" in msg

    mixed = {
        ("movie", "1"): {"name": None, "ok": True, "from_cache": True},
        ("show", "2"): {"name": None, "ok": True, "from_cache": False},
    }
    msg2 = _format_summary("User 5", "refresh", mixed)
    assert "from cache" not in msg2, "a mixed batch (not everything cached) should not claim it was all from cache"


def test_reset_one_stat_only_affects_that_stat():
    db.bump_stat("total_requests", 10)
    db.bump_stat("unique_fetches_ever", 7)
    db.reset_stat("total_requests")
    stats = db.dashboard_stats()
    assert stats["total_requests"] == 0
    assert stats["unique_fetches_ever"] == 7, "resetting one stat must not touch a different one"


def test_reset_all_stats_zeroes_every_resettable_counter():
    db.bump_stat("total_requests", 5)
    db.bump_stat("tiles_refreshed_ever", 3)
    db.bump_stat("tiles_fetched_ever", 2)
    db.reset_all_stats()
    stats = db.dashboard_stats()
    assert stats["total_requests"] == 0
    assert stats["tiles_refreshed_ever"] == 0
    assert stats["tiles_fetched_ever"] == 0


def test_reset_all_clears_the_cache_but_never_installation_history():
    # Reinstates the original V0.0.11 rule. It was briefly reversed because
    # the number was wrong — 22 installations when one person had ever run the
    # app — but that was a counting bug, since any X-Client-ID string used
    # while debugging registered as one. Ids are issued and signed now, so the
    # figure is real, and it is the one number here that cannot be recreated
    # by waiting; everything else is a tally that starts filling again at once.
    db.cache_set("some-key", "GET", "/3/search/movie", b'{"ok": true}', 200, ttl_seconds=3600)
    id_a = db.issue_client_id()
    num_a = db.touch_client(id_a)
    db.touch_client(db.issue_client_id())

    before = db.dashboard_stats()
    assert before["tiles_currently_cached"] == 1
    assert before["total_installations_ever"] == 2

    db.reset_all_stats()

    after = db.dashboard_stats()
    assert after["tiles_currently_cached"] == 0, "the cache itself must actually be cleared"
    assert after["total_installations_ever"] == 2, "installation history must survive"
    assert after["active_installations_30d"] == 2
    assert db.touch_client(id_a) == num_a, "an existing installation's number must not change"


def test_reset_all_never_touches_corrections_bugs_or_requests():
    # Those are things somebody wrote down, not counters. A button about
    # numbers must not delete work.
    db.override_set("tvmaze:1", "tvmaze", "Corrected Show", "21:00", 0, 4, 2027)
    db.feedback_add("bug", "installation-A", "something is broken", "5.4.6")
    db.feedback_add("request", "installation-B", "please add this", "5.4.6")
    db.report_add("tvmaze:1", "installation-A", "Corrected Show", "tvmaze")

    db.reset_all_stats()

    overrides = db.overrides_all()
    assert len(overrides) == 1
    assert overrides[0]["air_time"] == "21:00"
    assert overrides[0]["window_month"] == 4 and overrides[0]["window_year"] == 2027
    assert [e["body"] for e in db.feedback_list("bug")] == ["something is broken"]
    assert [e["body"] for e in db.feedback_list("request")] == ["please add this"]
    assert len(db.reports_summary()) == 1


def test_reset_all_actually_shrinks_the_database_file_via_vacuum():
    # Fill the cache with enough data that the file measurably grows,
    # then confirm resetting actually shrinks it back down again —
    # simply deleting rows without VACUUM would leave the file the same
    # size, since SQLite doesn't release freed pages back to the OS on
    # its own.
    big_body = b"x" * 200_000
    for i in range(20):
        db.cache_set(f"bulk-key-{i}", "GET", f"/3/movie/{i}", big_body, 200, ttl_seconds=3600)

    size_before = db.dashboard_stats()["db_file_bytes"]
    db.reset_all_stats()
    size_after = db.dashboard_stats()["db_file_bytes"]

    assert size_after < size_before, (
        f"expected the file to actually shrink after clearing {20 * len(big_body)} bytes of cache data "
        f"and VACUUMing, but went from {size_before} to {size_after} bytes"
    )


def test_reset_rejects_a_non_resettable_stat_name():
    with pytest.raises(ValueError):
        db.reset_stat("next_friendly_number")  # this one must never be reset by hand — it'd cause duplicate User Ns


@pytest.mark.asyncio
async def test_admin_reset_endpoints_require_login():
    async with await _client() as c:
        r1 = await c.post("/admin/reset-stat", data={"stat_name": "total_requests"})
        assert r1.status_code == 401
        r2 = await c.post("/admin/reset-all-stats")
        assert r2.status_code == 401


@pytest.mark.asyncio
async def test_admin_reset_one_stat_endpoint_works_when_logged_in():
    db.bump_stat("total_requests", 9)
    async with await _client() as c:
        await c.post("/admin/login", data={"password": "test-admin-pw"})
        r = await c.post("/admin/reset-stat", data={"stat_name": "total_requests"})
        assert r.status_code == 200
    assert db.dashboard_stats()["total_requests"] == 0


@pytest.mark.asyncio
async def test_dashboard_stream_requires_login():
    async with await _client() as c:
        r = await c.get("/admin/dashboard/stream")
        assert r.status_code == 401


@pytest.mark.asyncio
async def test_dashboard_stream_delivers_updated_stats_on_change():
    import json
    from app import stats_stream
    async with await _client() as c:
        await c.post("/admin/login", data={"password": "test-admin-pw"})

        queue = stats_stream.subscribe()
        try:
            db.bump_stat("total_requests", 1)
            payload = await asyncio.wait_for(queue.get(), timeout=1.0)
            data = json.loads(payload)
            assert "total_requests" in data
        finally:
            stats_stream.unsubscribe(queue)


@pytest.mark.asyncio
async def test_collection_id_endpoint_is_whitelisted_search_collection_is_not():
    async with await _client() as c:
        r1 = await c.get("/tmdb/collection/86311", headers=AUTH_HEADER)
        assert r1.status_code == 200
        # search/collection was the earlier fuzzy auto-detection endpoint,
        # removed entirely — the client now only ever fetches an exact,
        # hand-picked collection ID directly.
        r2 = await c.get("/tmdb/search/collection", params={"query": "avengers"}, headers=AUTH_HEADER)
        assert r2.status_code == 404


def test_describe_collection_results():
    from app.describe import describe_tmdb_request
    desc = describe_tmdb_request("collection/86311", {})
    assert desc == "collection results for #86311"


def test_collection_requests_are_treated_as_search_type_not_batched():
    from app.describe import is_tmdb_search_request
    assert is_tmdb_search_request("collection/86311") is True
    assert is_tmdb_search_request("search/collection") is False


def test_collection_names_learned_from_details_response():
    from app import titles
    from app.describe import describe_tmdb_request

    # Learnable directly from the collection-details response itself —
    # there's no separate search/collection step anymore to learn from.
    titles.remember_from_tmdb_response(
        "collection/10", b'{"id": 10, "name": "Star Wars Collection", "parts": []}'
    )
    assert describe_tmdb_request("collection/10", {}) == 'collection results for "Star Wars Collection"'


def test_credits_request_is_classified_separately_from_search_and_refresh():
    from app.describe import is_tmdb_search_request, is_tmdb_credits_request
    # A credits request is not a search...
    assert is_tmdb_search_request("movie/27205/credits") is False
    # ...but IS specifically recognized as credits, not a plain detail/refresh.
    assert is_tmdb_credits_request("movie/27205/credits") is True
    assert is_tmdb_credits_request("tv/1416/credits") is True
    # Plain detail and season requests are NOT credits requests.
    assert is_tmdb_credits_request("movie/27205") is False
    assert is_tmdb_credits_request("tv/1416/season/2") is False


@pytest.mark.asyncio
async def test_credits_burst_for_search_results_is_never_labeled_as_a_refresh():
    # Regression test for exactly the reported confusion: fetchCreditsForResults
    # fires one /credits call per displayed search result, and these were
    # being mislabeled as "Refreshed N tiles" even though nothing was
    # actually being refreshed — just cast/crew info loaded for a search.
    from app import refresh_batch
    original_delay = refresh_batch.FLUSH_DELAY_SECONDS
    refresh_batch.FLUSH_DELAY_SECONDS = 0.05
    try:
        header = issued_header()
        async with await _client() as c:
            await c.get("/tmdb/movie/111/credits", headers=header)
            await c.get("/tmdb/tv/222/credits", headers=header)
            await asyncio.sleep(0.15)

        log = db.get_recent_console_log(limit=50)
        messages = [e["message"] for e in log]

        assert not any("Refreshed" in m for m in messages), \
            "a credits burst must never be labeled as a tile refresh"
        assert any("Loaded cast & crew for 2 search results" in m for m in messages)
    finally:
        refresh_batch.FLUSH_DELAY_SECONDS = original_delay


@pytest.mark.asyncio
async def test_credits_batch_and_refresh_batch_stay_completely_separate():
    # Even for the SAME user, happening around the same time, a credits
    # burst and a genuine tile-refresh burst must produce two distinct
    # summary lines, never merged into one confusing message.
    from app import refresh_batch
    original_delay = refresh_batch.FLUSH_DELAY_SECONDS
    refresh_batch.FLUSH_DELAY_SECONDS = 0.05
    try:
        header = issued_header()
        async with await _client() as c:
            await c.get("/tmdb/movie/301/credits", headers=header)   # credits
            await c.get("/tmdb/movie/302", headers=header)           # genuine refresh
            await asyncio.sleep(0.15)

        log = db.get_recent_console_log(limit=50)
        messages = [e["message"] for e in log]

        refreshed_lines = [m for m in messages if "Refreshed" in m]
        credits_lines = [m for m in messages if "Loaded cast & crew" in m]
        assert len(refreshed_lines) == 1
        assert len(credits_lines) == 1
        assert "1 tile" in refreshed_lines[0]
        assert "1 search result" in credits_lines[0]
    finally:
        refresh_batch.FLUSH_DELAY_SECONDS = original_delay


# ── Feedback: bug reports and feature requests (V5.4.3) ─────────────────────

async def _login(c):
    """Log an already-open client into the admin session."""
    await c.post("/admin/login", data={"password": "test-admin-pw"})


@pytest.mark.asyncio
async def test_feedback_requires_auth():
    async with await _client() as c:
        r = await c.post("/feedback", json={"kind": "bug", "body": "x"})
        assert r.status_code == 401


@pytest.mark.asyncio
async def test_feedback_stores_a_bug_and_the_admin_can_read_it():
    async with await _client() as c:
        r = await c.post("/feedback",
                         json={"kind": "bug", "body": "Tile shows the wrong date",
                               "app_version": "5.4.3"},
                         headers=AUTH_HEADER)
        assert r.status_code == 200
        assert r.json() == {"ok": True, "accepted": True}

    async with await _client() as c:
        await _login(c)
        j = (await c.get("/admin/feedback/list?kind=bug")).json()
        assert len(j["entries"]) == 1
        e = j["entries"][0]
        assert e["body"] == "Tile shows the wrong date"
        assert e["app_version"] == "5.4.3"
        assert e["read"] == 0
        assert j["unread"]["bug"] == 1
        # …and it did NOT land in the other inbox.
        assert (await c.get("/admin/feedback/list?kind=request")).json()["entries"] == []


@pytest.mark.asyncio
async def test_feedback_kinds_are_separate_inboxes():
    async with await _client() as c:
        await c.post("/feedback", json={"kind": "request", "body": "Add dark mode"},
                     headers=AUTH_HEADER)
    async with await _client() as c:
        await _login(c)
        j = (await c.get("/admin/feedback/list?kind=request")).json()
        assert [e["body"] for e in j["entries"]] == ["Add dark mode"]
        assert j["unread"] == {"bug": 0, "request": 1}


@pytest.mark.asyncio
async def test_feedback_rejects_bad_kind_and_empty_body():
    async with await _client() as c:
        assert (await c.post("/feedback", json={"kind": "rant", "body": "x"},
                             headers=AUTH_HEADER)).status_code == 400
        assert (await c.post("/feedback", json={"kind": "bug", "body": "   "},
                             headers=AUTH_HEADER)).status_code == 400
        assert (await c.post("/feedback", json={"kind": "bug", "body": "y" * 5000},
                             headers=AUTH_HEADER)).status_code == 400


@pytest.mark.asyncio
async def test_feedback_has_no_cooldown():
    # Unlike the air-time report button — that one is a counted vote, so a
    # repeat has to be refused. This is a mailbox: several messages in a row
    # is a normal way to work through a list of problems, and refusing them
    # would throw away information someone freely offered.
    async with await _client() as c:
        for body in ("one", "two", "three"):
            r = await c.post("/feedback", json={"kind": "bug", "body": body},
                             headers=AUTH_HEADER)
            assert r.status_code == 200
            assert r.json()["accepted"] is True

    async with await _client() as c:
        await _login(c)
        bodies = [e["body"] for e in
                  (await c.get("/admin/feedback/list?kind=bug")).json()["entries"]]
        assert sorted(bodies) == ["one", "three", "two"]


@pytest.mark.asyncio
async def test_feedback_read_and_delete():
    async with await _client() as c:
        await c.post("/feedback", json={"kind": "bug", "body": "fix me"},
                     headers=AUTH_HEADER)

    async with await _client() as c:
        await _login(c)
        entry_id = (await c.get("/admin/feedback/list?kind=bug")).json()["entries"][0]["id"]

        r = await c.post("/admin/feedback/read", data={"entry_id": entry_id, "read": 1})
        assert r.json()["unread"]["bug"] == 0
        assert (await c.get("/admin/feedback/list?kind=bug")).json()["entries"][0]["read"] == 1

        # Unread again — a mailbox lets you put one back.
        await c.post("/admin/feedback/read", data={"entry_id": entry_id, "read": 0})
        assert (await c.get("/admin/feedback/list?kind=bug")).json()["unread"]["bug"] == 1

        await c.post("/admin/feedback/delete", data={"entry_id": entry_id})
        assert (await c.get("/admin/feedback/list?kind=bug")).json()["entries"] == []


@pytest.mark.asyncio
async def test_feedback_admin_endpoints_require_login():
    async with await _client() as c:
        assert (await c.get("/admin/feedback/list?kind=bug")).status_code == 401
        assert (await c.post("/admin/feedback/read",
                             data={"entry_id": 1, "read": 1})).status_code == 401
        assert (await c.post("/admin/feedback/delete",
                             data={"entry_id": 1})).status_code == 401
        for page in ("/admin/bugs", "/admin/requests"):
            assert (await c.get(page, follow_redirects=False)).status_code in (302, 303, 307)


@pytest.mark.asyncio
async def test_feedback_pages_render_and_show_the_message():
    async with await _client() as c:
        await c.post("/feedback", json={"kind": "request", "body": "picture in picture"},
                     headers=AUTH_HEADER)
    async with await _client() as c:
        await _login(c)
        r = await c.get("/admin/requests")
        assert r.status_code == 200
        assert "Requests" in r.text
        assert (await c.get("/admin/bugs")).status_code == 200


# ── Manual release windows on a correction (V5.4.5) ─────────────────────────

@pytest.mark.asyncio
async def test_override_window_month_and_year_round_trip():
    async with await _client() as c:
        await _login(c)
        r = await c.post("/admin/shows/override", data={
            "show_key": "tvmaze:1", "source": "tvmaze", "title": "Invincible",
            "air_time": "", "day_shift": 0,
            "window_month": "4", "window_year": "2027"})
        assert r.json()["ok"] is True

    async with await _client() as c:
        j = (await c.get("/overrides", headers=AUTH_HEADER)).json()
        e = j["overrides"][0]
        assert e["window_month"] == 4 and e["window_year"] == 2027
        assert e["air_time"] is None      # a window must not invent a time


@pytest.mark.asyncio
async def test_override_window_year_only_when_month_blank():
    async with await _client() as c:
        await _login(c)
        await c.post("/admin/shows/override", data={
            "show_key": "tvmaze:2", "source": "tvmaze", "title": "Some Show",
            "air_time": "", "day_shift": 0,
            "window_month": "", "window_year": "2028"})
    async with await _client() as c:
        e = (await c.get("/overrides", headers=AUTH_HEADER)).json()["overrides"][0]
        assert e["window_year"] == 2028
        assert e["window_month"] is None


@pytest.mark.asyncio
async def test_override_month_without_year_is_ignored():
    # "March" of which year? A month alone can't produce a window, and a
    # correction with nothing usable in it is not a correction.
    async with await _client() as c:
        await _login(c)
        await c.post("/admin/shows/override", data={
            "show_key": "tvmaze:3", "source": "tvmaze", "title": "No Year",
            "air_time": "", "day_shift": 0,
            "window_month": "3", "window_year": ""})
    async with await _client() as c:
        assert (await c.get("/overrides", headers=AUTH_HEADER)).json()["overrides"] == []


@pytest.mark.asyncio
async def test_override_window_rejects_bad_values():
    async with await _client() as c:
        await _login(c)
        base = {"show_key": "tvmaze:4", "source": "tvmaze", "title": "X",
                "air_time": "", "day_shift": 0}
        assert (await c.post("/admin/shows/override",
                             data={**base, "window_year": "27"})).status_code == 400
        assert (await c.post("/admin/shows/override",
                             data={**base, "window_year": "1200"})).status_code == 400
        assert (await c.post("/admin/shows/override",
                             data={**base, "window_month": "13",
                                   "window_year": "2027"})).status_code == 400


@pytest.mark.asyncio
async def test_clearing_everything_removes_the_correction():
    async with await _client() as c:
        await _login(c)
        await c.post("/admin/shows/override", data={
            "show_key": "tvmaze:5", "source": "tvmaze", "title": "Gone",
            "air_time": "21:00", "day_shift": 0,
            "window_month": "4", "window_year": "2027"})
        await c.post("/admin/shows/override", data={
            "show_key": "tvmaze:5", "source": "tvmaze", "title": "Gone",
            "air_time": "", "day_shift": 0,
            "window_month": "", "window_year": ""})
    async with await _client() as c:
        assert (await c.get("/overrides", headers=AUTH_HEADER)).json()["overrides"] == []


# ── Feedback to-do (V5.4.5) ─────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_feedback_todo_flag_also_marks_read():
    async with await _client() as c:
        await c.post("/feedback", json={"kind": "bug", "body": "fix this"},
                     headers=AUTH_HEADER)
    async with await _client() as c:
        await _login(c)
        e = (await c.get("/admin/feedback/list?kind=bug")).json()["entries"][0]
        assert e["todo"] == 0 and e["read"] == 0

        r = await c.post("/admin/feedback/todo", data={"entry_id": e["id"], "todo": 1})
        assert r.json()["unread"]["bug"] == 0        # deciding to act implies reading

        e = (await c.get("/admin/feedback/list?kind=bug")).json()["entries"][0]
        assert e["todo"] == 1 and e["read"] == 1

        await c.post("/admin/feedback/todo", data={"entry_id": e["id"], "todo": 0})
        e = (await c.get("/admin/feedback/list?kind=bug")).json()["entries"][0]
        assert e["todo"] == 0
        assert e["read"] == 1                        # unflagging doesn't un-read it


@pytest.mark.asyncio
async def test_feedback_todo_requires_login():
    async with await _client() as c:
        assert (await c.post("/admin/feedback/todo",
                             data={"entry_id": 1, "todo": 1})).status_code == 401


@pytest.mark.asyncio
async def test_register_is_limited_by_address():
    # /register is the one route with no client id to key on, which made it the
    # way around every other limit: mint ids in a loop and each one arrives with
    # its own full per-installation budget. A real install registers once ever.
    rate_limit._register_windows.clear()
    async with await _client() as c:
        allowed = 0
        for _ in range(rate_limit.REGISTER_MAX_PER_HOUR + 5):
            r = await c.post("/register", headers={"Authorization": "Bearer test-secret-123"})
            if r.status_code == 200:
                allowed += 1
            else:
                assert r.status_code == 429
        assert allowed == rate_limit.REGISTER_MAX_PER_HOUR

    # A different address gets its own allowance — one household hitting the
    # limit must not lock everybody else out.
    async with await _client() as c:
        r = await c.post("/register", headers={"Authorization": "Bearer test-secret-123",
                                               "X-Forwarded-For": "203.0.113.7"})
        assert r.status_code == 200
    rate_limit._register_windows.clear()


@pytest.mark.asyncio
async def test_register_still_refuses_a_bad_secret_before_counting():
    rate_limit._register_windows.clear()
    async with await _client() as c:
        r = await c.post("/register", headers={"Authorization": "Bearer wrong"})
        assert r.status_code == 401
    rate_limit._register_windows.clear()


def test_forwarded_address_uses_the_entry_caddy_added():
    # Caddy appends the peer it actually saw, so the LAST entry is the trusted
    # one; anything earlier could have been sent by the caller.
    class FakeRequest:
        headers = {"x-forwarded-for": "1.2.3.4, 5.6.7.8, 198.51.100.9"}
        client = None
    assert rate_limit.client_address(FakeRequest()) == "198.51.100.9"


def test_every_path_that_opens_the_editor_carries_every_correction_column():
    # The Active list and an alert row open the SAME editor, so both have to
    # hand it every column a correction is made of. When one of them was
    # missing a column the editor opened blank there and saved that blankness
    # back. Adding a column to OVERRIDE_COLUMNS without teaching a path to
    # select it now fails here rather than quietly eating somebody's data.
    db.override_set("tvmaze:900", "tvmaze", "Some Show", "21:00", 1, 3, 2026)
    db.report_add("tvmaze:900", "installation-A", "Some Show", "tvmaze")

    from_active = db.overrides_all()[0]
    from_alert = db.reports_summary()[0]

    for column in db.OVERRIDE_COLUMNS:
        assert column in from_active, f"Active list is missing {column}"
        assert column in from_alert, f"alert row is missing {column}"
        assert from_active[column] == from_alert[column], (
            f"{column} disagrees between the two paths")

    # And the no-correction-yet case a search hit uses.
    assert set(db.override_fields(None)) == set(db.OVERRIDE_COLUMNS)


def test_an_alert_carries_the_whole_correction_not_just_the_air_time():
    # An alert row on the Corrections page is a full editor for that show, so
    # it has to open holding everything already set. When the release window
    # was left out of this summary the row opened with an empty window box and
    # saving the air time from it wrote that emptiness back — which is how
    # Daredevil: Born Again ended up with an air time and no window despite
    # both being set.
    db.override_set("tvmaze:63234", "tvmaze", "Daredevil: Born Again",
                    "21:00", 0, 3, 2026)
    db.report_add("tvmaze:63234", "installation-A", "Daredevil: Born Again", "tvmaze")

    row = db.reports_summary()[0]

    assert row["air_time"] == "21:00"
    assert row["window_month"] == 3
    assert row["window_year"] == 2026


# ── A window correction is spent once a real date exists (V5.4.6) ───────────

async def _set_override(c, key, **kw):
    data = {"show_key": key, "source": "tvmaze", "title": "X",
            "air_time": "", "day_shift": 0, "window_month": "", "window_year": ""}
    data.update(kw)
    return await c.post("/admin/shows/override", data=data)


@pytest.mark.asyncio
async def test_window_superseded_removes_a_window_only_correction():
    async with await _client() as c:
        await _login(c)
        await _set_override(c, "tvmaze:1", window_month="4", window_year="2027")

    async with await _client() as c:
        r = await c.post("/window-superseded",
                         json={"show_key": "tvmaze:1", "real_date": "2027-04-09",
                               "title": "Invincible"},
                         headers=AUTH_HEADER)
        assert r.json() == {"ok": True, "cleared": True}
        assert (await c.get("/overrides", headers=AUTH_HEADER)).json()["overrides"] == []


@pytest.mark.asyncio
async def test_window_superseded_keeps_the_air_time():
    # The whole point of the rule: a window is a guess about WHEN a season
    # returns and expires with it, while an air time describes a slot the show
    # airs in and survives into the next season.
    async with await _client() as c:
        await _login(c)
        await _set_override(c, "tvmaze:2", air_time="21:00",
                            window_month="4", window_year="2027")

    async with await _client() as c:
        r = await c.post("/window-superseded",
                         json={"show_key": "tvmaze:2", "real_date": "2027-04-09"},
                         headers=AUTH_HEADER)
        assert r.json()["cleared"] is True
        e = (await c.get("/overrides", headers=AUTH_HEADER)).json()["overrides"][0]
        assert e["air_time"] == "21:00"
        assert e["window_year"] is None and e["window_month"] is None


@pytest.mark.asyncio
async def test_window_superseded_is_a_noop_without_a_window():
    async with await _client() as c:
        await _login(c)
        await _set_override(c, "tvmaze:3", air_time="21:00")

    async with await _client() as c:
        r = await c.post("/window-superseded",
                         json={"show_key": "tvmaze:3", "real_date": "2027-04-09"},
                         headers=AUTH_HEADER)
        assert r.json()["cleared"] is False
        # …and it certainly didn't take the air time with it.
        e = (await c.get("/overrides", headers=AUTH_HEADER)).json()["overrides"][0]
        assert e["air_time"] == "21:00"


@pytest.mark.asyncio
async def test_window_superseded_validates_its_input():
    async with await _client() as c:
        assert (await c.post("/window-superseded",
                             json={"show_key": "", "real_date": "2027-04-09"},
                             headers=AUTH_HEADER)).status_code == 400
        assert (await c.post("/window-superseded",
                             json={"show_key": "tvmaze:9", "real_date": "soon"},
                             headers=AUTH_HEADER)).status_code == 400
        assert (await c.post("/window-superseded",
                             json={"show_key": "tvmaze:9"},
                             headers=AUTH_HEADER)).status_code == 400


@pytest.mark.asyncio
async def test_window_superseded_requires_auth():
    async with await _client() as c:
        assert (await c.post("/window-superseded",
                             json={"show_key": "tvmaze:1",
                                   "real_date": "2027-04-09"})).status_code == 401


@pytest.mark.asyncio
async def test_window_superseded_twice_is_harmless():
    async with await _client() as c:
        await _login(c)
        await _set_override(c, "tvmaze:4", window_month="4", window_year="2027")
    async with await _client() as c:
        first = await c.post("/window-superseded",
                             json={"show_key": "tvmaze:4", "real_date": "2027-04-09"},
                             headers=AUTH_HEADER)
        second = await c.post("/window-superseded",
                              json={"show_key": "tvmaze:4", "real_date": "2027-04-09"},
                              headers=AUTH_HEADER)
        assert first.json()["cleared"] is True
        assert second.json()["cleared"] is False


# ── Relay-issued client ids (V0.0.13) ───────────────────────────────────────

@pytest.mark.asyncio
async def test_register_hands_out_a_verifiable_id():
    async with await _client() as c:
        r = await c.post("/register", headers={"Authorization": "Bearer test-secret-123"})
        assert r.status_code == 200
        cid = r.json()["client_id"]
        assert db.verify_client_id(cid)
        # Registering alone does not create an installation — using it does.
        assert db.dashboard_stats()["total_installations_ever"] == 0


@pytest.mark.asyncio
async def test_register_still_needs_the_shared_secret():
    async with await _client() as c:
        assert (await c.post("/register")).status_code == 401
        assert (await c.post("/register",
                             headers={"Authorization": "Bearer nope"})).status_code == 401


@pytest.mark.asyncio
async def test_a_made_up_client_id_is_served_but_never_counted():
    # This is the actual bug: every throwaway X-Client-ID used with curl while
    # debugging — test-1, xmen-debug, pick2, probe — became a permanent
    # installation, so the dashboard read 22 for one real user.
    made_up = {"Authorization": "Bearer test-secret-123", "X-Client-ID": "probe"}
    async with await _client() as c:
        r = await c.get("/tmdb/search/multi", params={"query": "x"}, headers=made_up)
        assert r.status_code == 200, "an unverified caller is still served"
    assert db.dashboard_stats()["total_installations_ever"] == 0, "…but is not an installation"


@pytest.mark.asyncio
async def test_an_issued_id_is_counted_once_however_often_it_is_used():
    async with await _client() as c:
        r = await c.post("/register", headers={"Authorization": "Bearer test-secret-123"})
        h = {"Authorization": "Bearer test-secret-123", "X-Client-ID": r.json()["client_id"]}
        for i in range(3):
            await c.get("/tmdb/search/multi", params={"query": f"q{i}"}, headers=h)
    stats = db.dashboard_stats()
    assert stats["total_installations_ever"] == 1
    assert stats["total_requests"] >= 3


@pytest.mark.asyncio
async def test_an_edited_id_does_not_verify():
    # "Changing your own id will not stick": the signature is over the random
    # half, so altering either half breaks it — including swapping in a
    # plausible-looking one to try to merge with somebody else.
    cid = db.issue_client_id()
    raw, sig = cid.split(".")
    assert db.verify_client_id(cid)
    assert not db.verify_client_id(raw[:-1] + ("0" if raw[-1] != "0" else "1") + "." + sig)
    assert not db.verify_client_id(raw + "." + ("0" * 16))
    assert not db.verify_client_id(raw)
    assert not db.verify_client_id("")
    assert not db.verify_client_id("11111111111111111111111111111111.2222222222222222")


@pytest.mark.asyncio
async def test_issued_ids_are_unique():
    ids = {db.issue_client_id() for _ in range(50)}
    assert len(ids) == 50


@pytest.mark.asyncio
async def test_unregistered_callers_are_labelled_as_such_in_the_console():
    made_up = {"Authorization": "Bearer test-secret-123", "X-Client-ID": "curl-debug"}
    async with await _client() as c:
        await c.get("/tmdb/search/multi", params={"query": "hello"}, headers=made_up)
    log = " ".join(e["message"] for e in db.get_recent_console_log(limit=20))
    assert "unregistered client" in log
    assert "User 0" not in log

@pytest.mark.asyncio
async def test_override_air_time_carries_its_zone():
    """An air time without a zone is ambiguous; one must always come back."""
    async with await _client() as c:
        await _login(c)
        r = await c.post("/admin/shows/override", data={
            "show_key": "tvmaze:900", "source": "tvmaze", "title": "Zoned Show",
            "air_time": "03:00", "day_shift": 0,
            "air_time_zone": "America/New_York",
            "window_month": "", "window_year": ""})
        assert r.json()["ok"] is True

    async with await _client() as c:
        j = (await c.get("/overrides", headers=AUTH_HEADER)).json()
        e = next(o for o in j["overrides"] if o["show_key"] == "tvmaze:900")
        assert e["air_time"] == "03:00"
        assert e["air_time_zone"] == "America/New_York"


@pytest.mark.asyncio
async def test_override_zone_defaults_to_eastern_when_absent():
    """
    An older client posts no zone at all. The correction must still be
    unambiguous rather than storing NULL and leaving the client to guess --
    which is the bug the column was added to remove.
    """
    async with await _client() as c:
        await _login(c)
        await c.post("/admin/shows/override", data={
            "show_key": "tvmaze:901", "source": "tvmaze", "title": "No Zone Sent",
            "air_time": "21:00", "day_shift": 0,
            "window_month": "", "window_year": ""})

    async with await _client() as c:
        j = (await c.get("/overrides", headers=AUTH_HEADER)).json()
        e = next(o for o in j["overrides"] if o["show_key"] == "tvmaze:901")
        assert e["air_time_zone"] == "America/New_York"


@pytest.mark.asyncio
async def test_override_rejects_unknown_zone_rather_than_storing_it():
    """
    A zone the app does not know would be ignored client-side, putting us back
    at an ambiguous time. Fall back to the default instead of keeping it.
    """
    async with await _client() as c:
        await _login(c)
        await c.post("/admin/shows/override", data={
            "show_key": "tvmaze:902", "source": "tvmaze", "title": "Bad Zone",
            "air_time": "12:00", "day_shift": 0,
            "air_time_zone": "Mars/Olympus_Mons",
            "window_month": "", "window_year": ""})

    async with await _client() as c:
        j = (await c.get("/overrides", headers=AUTH_HEADER)).json()
        e = next(o for o in j["overrides"] if o["show_key"] == "tvmaze:902")
        assert e["air_time_zone"] == "America/New_York"


@pytest.mark.asyncio
async def test_override_zone_reaches_the_alert_rows_too():
    """
    The handoff's recurring bug: the active list, the alert rows and a search
    hit all open the SAME editor, so a column missing from one of them opens
    blank there and is then SAVED blank -- that is how Daredevil: Born Again's
    window was wiped. OVERRIDE_COLUMNS is what keeps the three in step, so
    assert the new column is in it and that an alert row really carries it.
    """
    from app import db as _db
    assert "air_time_zone" in _db.OVERRIDE_COLUMNS
    assert _db.OVERRIDE_EMPTY["air_time_zone"] == "America/New_York"

    async with await _client() as c:
        await _login(c)
        await c.post("/admin/shows/override", data={
            "show_key": "tvmaze:903", "source": "tvmaze", "title": "Alerted Show",
            "air_time": "06:30", "day_shift": 0,
            "air_time_zone": "America/Chicago",
            "window_month": "", "window_year": ""})

    # A report on that show produces an alert row, which must carry the zone.
    _db.report_add("tvmaze:903", "client-zone-1", "Alerted Show", "tvmaze")
    rows = _db.reports_summary()
    row = next((r for r in rows if r["show_key"] == "tvmaze:903"), None)
    assert row is not None, "the report should produce an alert row"
    assert "air_time_zone" in row, "alert rows must carry every correction column"
    assert row["air_time_zone"] == "America/Chicago"
