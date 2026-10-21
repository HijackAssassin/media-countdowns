#pragma once

// The relay defaults for a build from the public source: deliberately BLANK.
// relayconfig.h uses relaysecret.local.h instead when that file exists (it is git-ignored).
// To bake your own relay into a private build, copy this file to relaysecret.local.h and fill
// both values in - never commit that file. See the comment in relayconfig.h.

#define RELAY_BASE_URL_VALUE      ""
#define RELAY_SHARED_SECRET_VALUE ""
