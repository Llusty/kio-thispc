/*
 * Per-location directory view preferences shared by both browser panes.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <QCryptographicHash>
#include <QSettings>
#include <QString>
#include <QUrl>

#include <algorithm>

class DirectoryViewSettings final
{
public:
    static constexpr int DefaultIconSizeMode = 1;
    static constexpr int NoGrouping = 0;
    static constexpr int GroupByType = 1;
    static constexpr int GroupByDate = 2;
    static constexpr int GroupBySize = 3;

    static int viewMode(const QUrl &rawUrl, int fallback)
    {
        const QUrl url = normalizedUrl(rawUrl);
        if (!url.isValid()) {
            return std::clamp(fallback, 0, 3);
        }

        QSettings settings;
        settings.beginGroup(QStringLiteral("directory/perLocationViewMode"));
        const QVariant stored = settings.value(keyForUrl(url));
        settings.endGroup();

        bool ok = false;
        const int mode = stored.toInt(&ok);
        return ok && mode >= 0 && mode <= 3
            ? mode
            : std::clamp(fallback, 0, 3);
    }

    static void setViewMode(const QUrl &rawUrl, int mode)
    {
        const QUrl url = normalizedUrl(rawUrl);
        if (!url.isValid()) {
            return;
        }

        QSettings settings;
        settings.beginGroup(QStringLiteral("directory/perLocationViewMode"));
        settings.setValue(keyForUrl(url), std::clamp(mode, 0, 3));
        settings.endGroup();
    }

    static int iconSizeMode(const QUrl &rawUrl, int fallback)
    {
        const QUrl url = normalizedUrl(rawUrl);
        if (!url.isValid()) {
            return std::clamp(fallback, 0, 3);
        }

        QSettings settings;
        settings.beginGroup(QStringLiteral("directory/perLocationIconSize"));
        const QVariant stored = settings.value(keyForUrl(url));
        settings.endGroup();

        bool ok = false;
        const int mode = stored.toInt(&ok);
        return ok && mode >= 0 && mode <= 3
            ? mode
            : std::clamp(fallback, 0, 3);
    }

    static void setIconSizeMode(const QUrl &rawUrl, int mode)
    {
        const QUrl url = normalizedUrl(rawUrl);
        if (!url.isValid()) {
            return;
        }

        QSettings settings;
        settings.beginGroup(QStringLiteral("directory/perLocationIconSize"));
        settings.setValue(keyForUrl(url), std::clamp(mode, 0, 3));
        settings.endGroup();
    }

    static int groupMode(const QUrl &rawUrl, int fallback)
    {
        const QUrl url = normalizedUrl(rawUrl);
        if (!url.isValid()) {
            return std::clamp(fallback, NoGrouping, GroupBySize);
        }
        QSettings settings;
        settings.beginGroup(QStringLiteral("directory/perLocationGrouping"));
        const QVariant stored = settings.value(keyForUrl(url));
        settings.endGroup();
        bool ok = false;
        const int mode = stored.toInt(&ok);
        return ok && mode >= NoGrouping && mode <= GroupBySize
            ? mode
            : std::clamp(fallback, NoGrouping, GroupBySize);
    }

    static void setGroupMode(const QUrl &rawUrl, int mode)
    {
        const QUrl url = normalizedUrl(rawUrl);
        if (!url.isValid()) return;
        QSettings settings;
        settings.beginGroup(QStringLiteral("directory/perLocationGrouping"));
        settings.setValue(keyForUrl(url), std::clamp(mode, NoGrouping, GroupBySize));
        settings.endGroup();
    }

    static QString keyForUrl(const QUrl &rawUrl)
    {
        const QUrl url = normalizedUrl(rawUrl).adjusted(
            QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
        const QByteArray encoded = url.toEncoded(QUrl::FullyEncoded);
        return QString::fromLatin1(
            QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex());
    }
};
