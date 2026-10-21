"""
Lightweight in-memory pub/sub so the console page can show new log
lines the instant they happen, via Server-Sent Events, instead of
waiting for a periodic full-page refresh.

Deliberately simple and in-memory only (not persisted, not shared
across multiple server processes) — this is just for "push this to
whichever browser tabs happen to be open on the console page right
now," which is exactly what it's for. The actual log itself is still
durably stored in SQLite (see db.py); this is purely the "notify live
viewers" side of it.
"""
import asyncio

_subscribers: set[asyncio.Queue] = set()


def subscribe() -> asyncio.Queue:
    q: asyncio.Queue = asyncio.Queue()
    _subscribers.add(q)
    return q


def unsubscribe(q: asyncio.Queue):
    _subscribers.discard(q)


def broadcast(message: str):
    for q in list(_subscribers):
        q.put_nowait(message)
