#pragma once

// =============================================================================
//  The notifier's version, in one place.
//
//  V5.4.25 — it had none: CMakeLists said 1.0.0 and had said so since the
//  program was written, while the main app was on 5.4.24. The two ship
//  together, are installed together and share tiles.json, so a bug report
//  naming "the notifier" was impossible to tie to a build.
//
//  It tracks the main app's number deliberately. They are one product, and two
//  independent version lines would mean working out which notifier went with
//  which app — the exact confusion this is meant to end.
//
//  Bump this and CMakeLists follows; nothing else needs changing.
// =============================================================================
#define MC_NOTIFIER_VERSION "5.4.28"
