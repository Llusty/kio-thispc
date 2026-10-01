/*
 * Pure URL parsing, validation, sanitization, and navigation semantics for remote locations.
 * Free of GUI / widget dependencies.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

namespace RemoteUrlHelper {

// Supported remote protocols for Stage 1 manual connections
bool isRemoteScheme(const QString &scheme);

// Returns true if URL has a valid remote scheme
bool isRemoteUrl(const QUrl &url);

// Returns true if URL is a native discovery URL (remote:/ or smb:/ without host)
bool isRemoteDiscoveryUrl(const QUrl &url);

// Returns true if URL is valid, has a remote scheme, and non-empty host
bool isValidRemoteUrl(const QUrl &url);

// Returns true if both remote URLs point to the same remote target (ignoring password/trailing slashes)
bool isSameRemoteLocation(const QUrl &a, const QUrl &b);

// Removes only password from URL. Preserves username, host, port, path, query, fragment.
QUrl sanitizeUrl(const QUrl &url);

// Parses user input string from address bar.
// If remote scheme is detected: validates host, strips password, normalizes trailing slash.
// If malformed (e.g. missing host): returns invalid QUrl().
QUrl parseUserInput(const QString &input);

// Computes parent URL for remote locations.
// For server root (path empty or "/"), returns kThisPcUrl (thispc:/).
// For nested paths, trims the deepest segment.
QUrl parentUrl(const QUrl &url);

// Label for root crumb (e.g. "user@host:port" or "host:port" or "host")
QString rootLabel(const QUrl &url);

// Server root URL (scheme://user@host:port/)
QUrl rootUrl(const QUrl &url);

// Returns recommended icon name for remote URL
QString iconForRemoteUrl(const QUrl &url);

// Sanitizes error messages from KIO or system to ensure no passwords leak
QString sanitizeErrorMessage(const QString &error, const QUrl &url = QUrl());

} // namespace RemoteUrlHelper
