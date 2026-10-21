@echo off
REM Starts the relay, and Caddy in front of it, each in its own minimised
REM window so you can read either one's output.
REM
REM Change the path below to wherever you put this folder.
REM
REM If you are not exposing the relay to the internet, delete the second line:
REM Caddy exists only to terminate HTTPS on a public hostname. Without it the
REM relay still serves on http://localhost:8080.

set RELAY_DIR=C:\Media Countdowns Relay Server

start "MC Relay - Server" /min cmd /k "cd /d %RELAY_DIR% && uvicorn app.main:app --host 0.0.0.0 --port 8080"
start "MC Relay - Caddy"  /min cmd /k "cd /d %RELAY_DIR% && caddy run"
