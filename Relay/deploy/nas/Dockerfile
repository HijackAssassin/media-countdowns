# =============================================================================
#  Media Countdowns Relay — container image for the WD My Cloud EX2 Ultra.
#
#  That NAS is a Marvell Armada 385: ARMv7, dual core at 1.3 GHz, 1 GB of RAM.
#  Two consequences shape this file.
#
#  1. The image must be linux/arm/v7. python:3.11-slim publishes that
#     architecture, so simply building ON the NAS gets the right one — there is
#     nothing to configure, and no cross-building to arrange.
#
#  2. Anything that has to be COMPILED is a problem. 1 GB of RAM shared with
#     the NAS's own services is not enough to build Rust crates, and at
#     1.3 GHz it would take hours before failing. requirements-nas.txt exists
#     to keep every dependency on the prebuilt-wheel path — read the note at
#     the top of it before changing a version here.
#
#  Build context is the REPOSITORY ROOT, not this folder, because the image
#  needs app/. See docker-compose.yml, which sets that up.
#
#  -------------------------------------------------------------------------
#  THE BASE IMAGE TAG IS LOAD-BEARING. Do not "modernise" it.
#
#  plain python:3.11-slim (Debian bookworm) FAILS on this NAS before pip even
#  starts, while importing Python's own logging module:
#
#      OSError: [Errno 75] Value too large for defined data type
#
#  Errno 75 is EOVERFLOW. A 32-bit process cannot stat() a file whose inode
#  number needs more than 32 bits, and Docker here uses the vfs storage driver
#  on a 12.6 TB ext4 volume, which hands out inode numbers well past that.
#  Nothing to do with this project's code or its dependencies.
#
#  Measured on the device, same one-line test in each:
#      python:3.11-slim           (bookworm)  EOVERFLOW on import logging
#      python:3.11-slim-bullseye              PermissionError, Errno 1
#      python:3.11-alpine                     no armv7 image published
#      python:3.10-alpine                     no armv7 image published
#      python:3.10-slim-buster                works, 3.10.12
#      python:3.11-slim-buster                works, 3.11.4   <-- this one
#
#  buster's older glibc does not overflow on those inodes. It is Debian 10 and
#  out of standard support, which is worth knowing: this image gets no OS
#  security updates. It is acceptable here because nothing in it is exposed
#  directly -- Caddy terminates TLS and is the only thing published -- the
#  container runs no shell services, and the parts that actually face the
#  network (Python and its packages) are still updated by rebuilding.
#
#  If the NAS is ever replaced with a 64-bit machine, this whole note stops
#  applying and the tag can go back to python:3.11-slim.
#  -------------------------------------------------------------------------
FROM python:3.11-slim-buster

# Unbuffered so `docker logs` shows output as it happens rather than in blocks,
# which matters when the only window onto this is Portainer's log view.
ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

WORKDIR /srv

# Dependencies first, in their own layer: they change far less often than the
# code does, so a code edit rebuilds in seconds instead of re-resolving pip.
COPY deploy/nas/requirements-nas.txt .
# --prefer-binary makes pip take a wheel whenever one exists rather than
# preferring a newer source release it would have to compile. On this hardware
# that preference is the difference between a build finishing and not.
RUN pip install --no-cache-dir --prefer-binary -r requirements-nas.txt

COPY app ./app

# The database lives on a mounted volume, NOT inside the image — otherwise
# every rebuild would throw away the installation history, the cache, and the
# client-id signing secret. app/db.py reads this variable and defaults to a
# relative "relay.db", which would land in /srv and vanish on redeploy.
ENV RELAY_DB_PATH=/data/relay.db
VOLUME ["/data"]

# Only Caddy talks to this; it is not published to the LAN. See the compose
# file — the relay has no `ports:` mapping of its own, on purpose.
EXPOSE 8080

# No --reload, no --workers. One worker is right: the relay's SQLite database
# is a single file with no connection pooling, and several worker processes
# writing to it would contend for locks for no gain at this request volume.
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8080"]
