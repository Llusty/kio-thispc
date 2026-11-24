/*
 * Portable XDG/Qt6 path helpers compatible with Qt 6.4.2 and newer.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QString>

namespace ThisPc::PortablePaths {

/**
 * Returns the writable generic state directory following the XDG Base Directory
 * Specification ($XDG_STATE_HOME if it starts with '/', otherwise $HOME/.local/state;
 * honors QStandardPaths::isTestModeEnabled()).
 * Uses QStandardPaths::GenericStateLocation on Qt 6.7+ and an XDG-compliant
 * fallback on Qt 6.4.2 - 6.6.x. Never falls back to root ("/" or "/.local/state").
 */
QString genericStateLocation();

/**
 * Pure deterministic resolver for application state directory given explicit
 * base directory, organization name, application name, and fallback subdirectory.
 * Rejects malformed, absolute, or traversal ("..") components.
 */
QString resolveAppStateLocation(const QString &genericBase,
                                const QString &orgName,
                                const QString &appName,
                                const QString &fallbackSubdir = QStringLiteral("kio-thispc/thispc-view"));

/**
 * Returns the writable application state directory.
 * When QCoreApplication organization/application metadata is set, resolves
 * <genericStateLocation>/<org>/<app> (matching QStandardPaths::StateLocation on
 * Qt 6.7+ and identical production history paths on all Qt 6.4.2+ versions).
 * When both organization and application metadata are empty, appends the
 * validated relative fallbackSubdir on both Qt 6.4.2 and Qt 6.7+; malformed,
 * absolute, or traversal ("..") fallbackSubdir values are rejected.
 * Falls back to QStandardPaths::AppDataLocation if no valid state base exists.
 */
QString appStateLocation(const QString &fallbackSubdir = QStringLiteral("kio-thispc/thispc-view"));

QString appDataLocation();
QString appCacheLocation();
QString appConfigLocation();

} // namespace ThisPc::PortablePaths
