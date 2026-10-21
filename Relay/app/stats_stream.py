"""
Lightweight in-memory pub/sub so the dashboard page's numbers update the
instant they change, via Server-Sent Events, instead of waiting for the
old 30-second page refresh. Mirrors live_log.py's exact pattern — kept
as a separate channel since this carries structured stats data (a JSON
object), not console log text.
"""
import asyncio
import json

_subscribers: set[asyncio.Queue] = set()


def subscribe() -> asyncio.Queue:
    q: asyncio.Queue = asyncio.Queue()
    _subscribers.add(q)
    return q


def unsubscribe(q: asyncio.Queue):
    _subscribers.discard(q)


def broadcast(stats: dict):
    payload = json.dumps(stats)
    for q in list(_subscribers):
        q.put_nowait(payload)
