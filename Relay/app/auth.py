"""
Client authentication for the relay, plus lightweight installation
tracking.

Auth is still a single shared secret for everyone — deliberately not
evolved into full per-user accounts, since that's a much bigger feature
than "let me see how many people are using this." Instead, each desktop
app installation generates its own random, anonymous ID once (no
personal info in it at all) and sends it alongside the shared secret —
that's what lets the dashboard count distinct installations, apply a
fair per-installation rate limit, and (as of the console log) give each
one a simple, human-friendly number like "User 365" instead of a raw ID.

If you later want per-user tokens (e.g. to distribute this to several
people and be able to revoke one person's access without affecting
everyone), swap this out for a small token table (even a flat file or
SQLite row per issued token) — the rest of the app doesn't care, it just
calls verify_client(token).
"""
from dataclasses import dataclass
from fastapi import Header, HTTPException
from app.config import settings
from app import db


@dataclass
class ClientIdentity:
    client_id: str        # the anonymous installation ID (used for rate limiting)
    friendly_number: int  # e.g. 365, for showing "User 365"; 0 when unregistered

    @property
    def label(self) -> str:
        """What the console log calls this caller."""
        return f"User {self.friendly_number}" if self.friendly_number else "unregistered client"


async def verify_client(
    authorization: str = Header(default=""),
    x_client_id: str = Header(default=""),
) -> ClientIdentity:
    if not settings.RELAY_SHARED_SECRET:
        # Server misconfigured — fail closed, not open.
        raise HTTPException(status_code=500, detail="Relay has no shared secret configured")

    expected = f"Bearer {settings.RELAY_SHARED_SECRET}"
    if authorization != expected:
        raise HTTPException(status_code=401, detail="Invalid or missing relay token")

    db.bump_stat("total_requests")

    # Only an id this relay issued counts as an installation.
    #
    # It used to register whatever arrived in the header, so every throwaway
    # value used while debugging with curl — test-1, xmen-debug, pick2, probe —
    # became a permanent "installation" and the dashboard read 22 when one
    # person had ever run the app. Anything unverified is still served
    # normally, because breaking a curl against your own relay to fix a
    # counting problem would be a poor trade; it simply isn't counted, and
    # /register is where a real client gets an id it can present.
    if db.verify_client_id(x_client_id):
        friendly_number = db.touch_client(x_client_id)
        return ClientIdentity(client_id=x_client_id, friendly_number=friendly_number)

    # Rate limiting still needs SOMETHING to key on. The raw header is used
    # as-is so an unregistered caller is still limited per-value rather than
    # sharing one bucket with every other unregistered caller.
    return ClientIdentity(client_id=x_client_id or "unregistered", friendly_number=0)
