#pragma once

// =============================================================================
//  The app's own version, in one place.
//
//  It used to be a `static const QString kCurrentVersion = "5.0.0"` buried
//  inside MainWindow::checkForUpdates(), which had been left behind at 5.0.0
//  through every release since — so the update check was comparing the relay's
//  published version against a number four minor releases old and would
//  announce an update that was already installed.
//
//  Bump this on release, and both the update check and the version stamped on
//  a bug report follow automatically.
// =============================================================================
#define MC_APP_VERSION "5.4.28"
