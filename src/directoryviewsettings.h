/*
 * Per-location directory view preferences shared by both browser panes.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <QCryptographicHash>
#include <QSettings>
#include <QString>
#include <QUrl>

#include <algorithm>

struct DirectoryViewProfile
{
    int viewMode = 0;
    int iconSizeStep = 5;
    int sortKey = 0;
    bool sortAscending = true;
    int groupMode = 0;
    bool previewsEnabled = true;

    bool operator==(const DirectoryViewProfile &) const = default;
};

class DirectoryViewSettings final
{
public:
    static constexpr int ProfileVersion = 3;
    static constexpr int DefaultIconSizeMode = 1;
    static constexpr int DefaultIconSizeStep = 5;
    static constexpr int NoGrouping = 0;
    static constexpr int GroupByType = 1;
    static constexpr int GroupByDate = 2;
    static constexpr int GroupBySize = 3;

    static DirectoryViewProfile globalDefault()
    {
        QSettings settings;
        return sanitized({settings.value(QStringLiteral("directory/viewMode"), 0).toInt(),
                          legacyModeToStep(settings.value(QStringLiteral("directory/iconSizeMode"), DefaultIconSizeMode).toInt()),
                          settings.value(QStringLiteral("directory/sortKey"), 0).toInt(),
                          settings.value(QStringLiteral("directory/sortAscending"), true).toBool(),
                          NoGrouping,
                          true});
    }

    static constexpr int iconSizeStepCount() { return 9; }

    static constexpr int iconExtentForStep(int step)
    {
        constexpr int extents[] = {24, 32, 40, 48, 56, 64, 80, 96, 128};
        return extents[std::clamp(step, 0, iconSizeStepCount() - 1)];
    }

    static int stepForIconExtent(int extent)
    {
        int best = 0;
        for (int step = 1; step < iconSizeStepCount(); ++step) {
            if (std::abs(iconExtentForStep(step) - extent)
                < std::abs(iconExtentForStep(best) - extent)) best = step;
        }
        return best;
    }

    static constexpr int legacyModeToStep(int mode)
    {
        constexpr int steps[] = {7, 5, 3, 1};
        return steps[std::clamp(mode, 0, 3)];
    }

    static DirectoryViewProfile resolveProfile(const QUrl &rawUrl)
    {
        const QUrl url = profileUrl(rawUrl);
        DirectoryViewProfile profile = globalDefault();
        if (!url.isValid()) return profile;
        if (readExplicit(url, &profile)) return profile;
        if (readLegacyExact(url, &profile)) return sanitized(profile);
        for (QUrl ancestor = parentUrl(url); ancestor.isValid(); ancestor = parentUrl(ancestor))
            if (readRule(ancestor, &profile)) return profile;
        return globalDefault();
    }

    static void saveExplicitProfile(const QUrl &rawUrl, const DirectoryViewProfile &rawProfile)
    {
        const QUrl url = profileUrl(rawUrl);
        if (!url.isValid()) return;
        QSettings settings;
        settings.beginGroup(profileGroup(url));
        settings.setValue(QStringLiteral("url"), url.toString(QUrl::FullyEncoded));
        settings.setValue(QStringLiteral("version"), ProfileVersion);
        writeProfile(settings, QStringLiteral("explicit"), sanitized(rawProfile));
        settings.endGroup();
    }

    static void setInheritedRule(const QUrl &rawUrl, const DirectoryViewProfile &rawProfile,
                                 bool enabled)
    {
        const QUrl url = profileUrl(rawUrl);
        if (!url.isValid()) return;
        QSettings settings;
        settings.beginGroup(profileGroup(url));
        settings.setValue(QStringLiteral("url"), url.toString(QUrl::FullyEncoded));
        settings.setValue(QStringLiteral("version"), ProfileVersion);
        settings.setValue(QStringLiteral("rule/enabled"), enabled);
        if (enabled) writeProfile(settings, QStringLiteral("rule"), sanitized(rawProfile));
        settings.endGroup();
    }

    static bool hasExplicitProfile(const QUrl &rawUrl)
    {
        DirectoryViewProfile ignored;
        const QUrl url = profileUrl(rawUrl);
        return readExplicit(url, &ignored) || hasLegacyExact(url);
    }

    static bool hasInheritedRule(const QUrl &rawUrl)
    {
        DirectoryViewProfile ignored;
        return readRule(profileUrl(rawUrl), &ignored);
    }

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

private:
    static DirectoryViewProfile sanitized(DirectoryViewProfile profile)
    {
        profile.viewMode = std::clamp(profile.viewMode, 0, 3);
        profile.iconSizeStep = std::clamp(profile.iconSizeStep, 0, iconSizeStepCount() - 1);
        profile.sortKey = std::clamp(profile.sortKey, 0, 3);
        profile.groupMode = std::clamp(profile.groupMode, NoGrouping, GroupBySize);
        return profile;
    }

    static QUrl profileUrl(const QUrl &rawUrl)
    {
        if (!rawUrl.isValid()) return {};
        return normalizedUrl(rawUrl).adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
    }

    static QUrl parentUrl(const QUrl &rawUrl)
    {
        const QUrl url = profileUrl(rawUrl);
        if (!url.isValid()) return {};
        const QString oldPath = url.path();
        if (oldPath.isEmpty() || oldPath == QStringLiteral("/")) return {};
        const int slash = oldPath.lastIndexOf(QLatin1Char('/'));
        QUrl parent = url;
        parent.setPath(slash <= 0 ? QStringLiteral("/") : oldPath.left(slash));
        parent.setQuery(QString());
        parent.setFragment(QString());
        parent = profileUrl(parent);
        return parent == url ? QUrl{} : parent;
    }

    static QString profileGroup(const QUrl &url)
    {
        return QStringLiteral("directory/profiles/%1").arg(keyForUrl(url));
    }

    static void writeProfile(QSettings &settings, const QString &prefix,
                             const DirectoryViewProfile &profile)
    {
        settings.setValue(prefix + QStringLiteral("/viewMode"), profile.viewMode);
        settings.setValue(prefix + QStringLiteral("/iconSizeStep"), profile.iconSizeStep);
        settings.setValue(prefix + QStringLiteral("/sortKey"), profile.sortKey);
        settings.setValue(prefix + QStringLiteral("/sortAscending"), profile.sortAscending);
        settings.setValue(prefix + QStringLiteral("/groupMode"), profile.groupMode);
        settings.setValue(prefix + QStringLiteral("/previewsEnabled"), profile.previewsEnabled);
    }

    static bool readProfile(QSettings &settings, const QString &prefix,
                            DirectoryViewProfile *profile)
    {
        if (!settings.contains(prefix + QStringLiteral("/viewMode"))) return false;
        const int version = settings.value(QStringLiteral("version"), 1).toInt();
        const int iconStep = version >= 2
            ? settings.value(prefix + QStringLiteral("/iconSizeStep"), DefaultIconSizeStep).toInt()
            : legacyModeToStep(settings.value(prefix + QStringLiteral("/iconSizeMode"), DefaultIconSizeMode).toInt());
        *profile = sanitized({settings.value(prefix + QStringLiteral("/viewMode")).toInt(),
                              iconStep,
                              settings.value(prefix + QStringLiteral("/sortKey"), 0).toInt(),
                              settings.value(prefix + QStringLiteral("/sortAscending"), true).toBool(),
                              settings.value(prefix + QStringLiteral("/groupMode"), NoGrouping).toInt(),
                              settings.value(prefix + QStringLiteral("/previewsEnabled"), true).toBool()});
        return true;
    }

    static bool readExplicit(const QUrl &url, DirectoryViewProfile *profile)
    {
        if (!url.isValid()) return false;
        QSettings settings;
        settings.beginGroup(profileGroup(url));
        const bool matches = settings.value(QStringLiteral("url")).toString()
            == url.toString(QUrl::FullyEncoded);
        const bool found = matches && readProfile(settings, QStringLiteral("explicit"), profile);
        settings.endGroup();
        return found;
    }

    static bool readRule(const QUrl &url, DirectoryViewProfile *profile)
    {
        if (!url.isValid()) return false;
        QSettings settings;
        settings.beginGroup(profileGroup(url));
        const bool matches = settings.value(QStringLiteral("url")).toString()
            == url.toString(QUrl::FullyEncoded);
        const bool found = matches && settings.value(QStringLiteral("rule/enabled"), false).toBool()
            && readProfile(settings, QStringLiteral("rule"), profile);
        settings.endGroup();
        return found;
    }

    static bool readLegacyValue(const QString &group, const QUrl &url, int minimum,
                                int maximum, int *value)
    {
        QSettings settings;
        settings.beginGroup(group);
        const QVariant stored = settings.value(keyForUrl(url));
        settings.endGroup();
        bool ok = false;
        const int candidate = stored.toInt(&ok);
        if (!stored.isValid() || !ok || candidate < minimum || candidate > maximum) return false;
        *value = candidate;
        return true;
    }

    static bool readLegacyExact(const QUrl &url, DirectoryViewProfile *profile)
    {
        if (!url.isValid()) return false;
        bool found = false;
        int value = 0;
        if (readLegacyValue(QStringLiteral("directory/perLocationViewMode"), url, 0, 3, &value)) {
            profile->viewMode = value; found = true;
        }
        if (readLegacyValue(QStringLiteral("directory/perLocationIconSize"), url, 0, 3, &value)) {
            profile->iconSizeStep = legacyModeToStep(value); found = true;
        }
        if (readLegacyValue(QStringLiteral("directory/perLocationGrouping"), url,
                            NoGrouping, GroupBySize, &value)) {
            profile->groupMode = value; found = true;
        }
        return found;
    }

    static bool hasLegacyExact(const QUrl &url)
    {
        DirectoryViewProfile profile = globalDefault();
        return readLegacyExact(url, &profile);
    }
};
