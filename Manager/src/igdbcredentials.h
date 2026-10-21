#pragma once
// =============================================================================
//  igdbcredentials.h — v3.3.1, cleared in V4
//
//  V4 onward: left intentionally blank. With the relay routing IGDB
//  calls by default, the app no longer needs any IGDB credentials
//  baked in at all — the relay holds the real ones. These two
//  constants only matter if relay mode is turned off in Settings and
//  no personal Client ID/Secret has been entered there either.
//
//  If you want a build-time default anyway (e.g. for your own private
//  fork), paste your own Client ID/Secret below, then rebuild — but
//  don't commit or share this file once it's filled in, since it would
//  then contain your real Client Secret in plain text, compiled
//  directly into the .exe. Settings -> IGDB API Credentials always
//  takes priority over these two lines regardless.
// =============================================================================
static constexpr const char* IGDB_CLIENT_ID_DEFAULT     = "";
static constexpr const char* IGDB_CLIENT_SECRET_DEFAULT = "";
