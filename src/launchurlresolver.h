/*
 * Pure launch URL resolution and safety contract for local/remote targets.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include <QDir>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace LaunchUrlResolver {

inline bool isSafeSubpath(const QStringList &segments)
{
    for (const QString &segment : segments) {
        if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String("..")) {
            return false;
        }
        // Reject encoded traversal or separator injection
        if (segment.contains(QLatin1Char('/')) || segment.contains(QLatin1Char('\\'))) {
            return false;
        }
        const QString lower = segment.toLower();
        if (lower.contains(QLatin1String("%2e")) || lower.contains(QLatin1String("%2f"))
            || lower.contains(QLatin1String("%5c"))) {
            return false;
        }
    }
    return true;
}

inline QUrl resolveLaunchUrl(
    const QUrl &rawUrl,
    const QList<DriveInfo> &drives,
    const QUrl &targetUrl = QUrl())
{
    if (!rawUrl.isValid() && !targetUrl.isValid()) {
        return rawUrl;
    }

    // A. Explicit targetUrl:
    if (targetUrl.isValid()) {
        if (targetUrl.isLocalFile()) {
            return targetUrl;
        }
        // Non-local targetUrl (e.g. sftp, smb) must never be converted to file://
        return targetUrl;
    }

    // B. rawUrl: if already local file -> use it
    if (rawUrl.isLocalFile()) {
        return rawUrl;
    }

    // Remote protocols (sftp, smb, fish, etc.) or any non-thispc scheme:
    // MUST NEVER be converted to file://
    if (rawUrl.scheme() != QStringLiteral("thispc")) {
        return rawUrl;
    }

    if (rawUrl.toString().contains(QLatin1String(".."))
        || rawUrl.toString().contains(QLatin1Char('\\'))) {
        return rawUrl;
    }

    const QString encodedPath = rawUrl.path(QUrl::FullyEncoded);
    if (encodedPath.contains(QLatin1String("%2e"), Qt::CaseInsensitive)
        || encodedPath.contains(QLatin1String("%2f"), Qt::CaseInsensitive)
        || encodedPath.contains(QLatin1String("%5c"), Qt::CaseInsensitive)) {
        return rawUrl;
    }

    // D. thispc:/<drive-id>/<subpath...>:
    // Only EXACT stable drive.id match to mounted drive.targetUrl
    const QString path = rawUrl.path();
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return rawUrl; // Root thispc:/ is not a local file
    }

    const QString &driveId = parts.first();
    for (const DriveInfo &drive : drives) {
        // EXACT match only: no case-insensitivity, no name, no udi, no fuzzy stripping
        if (drive.id == driveId) {
            if (!drive.isMounted) {
                return rawUrl; // Unmounted drives cannot map to local files
            }
            if (!drive.targetUrl.isValid() || !drive.targetUrl.isLocalFile()) {
                return rawUrl;
            }

            const QString mountRoot = drive.targetUrl.toLocalFile();
            if (mountRoot.isEmpty()) {
                return rawUrl;
            }

            if (parts.size() == 1) {
                return drive.targetUrl;
            }

            QStringList subSegments = parts;
            subSegments.removeFirst();

            if (!isSafeSubpath(subSegments)) {
                // Potential path traversal or escape detected: do not convert
                return rawUrl;
            }

            // Lexical composition
            QString combined = mountRoot;
            if (!combined.endsWith(QLatin1Char('/'))) {
                combined += QLatin1Char('/');
            }
            combined += subSegments.join(QLatin1Char('/'));

            // Clean path lexically
            const QString cleaned = QDir::cleanPath(combined);
            // Lexical descendant verification: cleaned must start with mountRoot
            const QString rootPrefix = mountRoot.endsWith(QLatin1Char('/'))
                ? mountRoot
                : (mountRoot + QLatin1Char('/'));
            if (!cleaned.startsWith(rootPrefix) && cleaned != mountRoot) {
                return rawUrl;
            }

            return QUrl::fromLocalFile(cleaned);
        }
    }

    // No exact drive match: leave rawUrl unchanged
    return rawUrl;
}

} // namespace LaunchUrlResolver
