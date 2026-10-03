/*
 * Data model for drive / filesystem properties in ThisPC.
 *
 * Part of Properties 2.0 (0.37.0 Stage 2).
 * Decouples drive/filesystem metadata model from presentation widgets.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <QLocale>
#include <QString>

struct DrivePropertiesData
{
    QString udi;
    QString parentDriveUdi;
    QString name;
    QString deviceNode;
    QString parentDriveNode;
    QString driveModel;
    QString driveVendor;
    QString driveBus;
    QString uuid;
    QString fileSystemType;
    QString volumeFsType;

    bool isMounted = false;
    QString mountPoint;
    QString mountOptions;

    bool isReadOnly = false;
    bool isRemovable = false;
    bool isHotpluggable = false;

    qint64 totalBytes = -1;
    qint64 freeBytes = -1;
    qint64 usedBytes = -1;
    int usedPercent = 0;

    bool isDeviceMissing = false;

    bool hasTotalBytes() const
    {
        return totalBytes >= 0;
    }

    bool hasFreeBytes() const
    {
        return isMounted && freeBytes >= 0;
    }

    bool hasUsedBytes() const
    {
        return isMounted && usedBytes >= 0;
    }

    QString formattedTotal() const
    {
        if (!hasTotalBytes()) {
            return isPolish() ? QStringLiteral("Niedostępne") : QStringLiteral("Unavailable");
        }
        const QString human = formatFileSize(totalBytes, false);
        const QString exact = QLocale().toString(totalBytes);
        return QStringLiteral("%1 (%2 %3)")
            .arg(human, exact, isPolish() ? QStringLiteral("bajtów") : QStringLiteral("bytes"));
    }

    QString formattedFree() const
    {
        if (!hasFreeBytes()) {
            return isPolish() ? QStringLiteral("Niedostępne") : QStringLiteral("Unavailable");
        }
        const QString human = formatFileSize(freeBytes, false);
        const QString exact = QLocale().toString(freeBytes);
        return QStringLiteral("%1 (%2 %3)")
            .arg(human, exact, isPolish() ? QStringLiteral("bajtów") : QStringLiteral("bytes"));
    }

    QString formattedUsed() const
    {
        if (!hasUsedBytes()) {
            return isPolish() ? QStringLiteral("Niedostępne") : QStringLiteral("Unavailable");
        }
        const QString human = formatFileSize(usedBytes, false);
        const QString exact = QLocale().toString(usedBytes);
        return QStringLiteral("%1 (%2 %3)")
            .arg(human, exact, isPolish() ? QStringLiteral("bajtów") : QStringLiteral("bytes"));
    }

    QString formattedFileSystem() const
    {
        const QString vol = volumeFsType.trimmed();
        const QString tech = fileSystemType.trimmed();

        if (vol.isEmpty() && tech.isEmpty()) {
            return isPolish() ? QStringLiteral("Nieznany") : QStringLiteral("Unknown");
        }
        if (vol.isEmpty()) {
            return tech;
        }
        if (tech.isEmpty() || vol.compare(tech, Qt::CaseInsensitive) == 0) {
            return vol.toUpper();
        }
        // Different types (e.g. volume is "ntfs" and technical is "fuseblk" or "ntfs3")
        return QStringLiteral("%1 (%2)").arg(vol.toUpper(), tech);
    }

    QString formattedMountOptions() const
    {
        if (mountOptions.trimmed().isEmpty()) {
            return QStringLiteral("—");
        }
        const QStringList parts = mountOptions.split(QLatin1Char(','), Qt::SkipEmptyParts);
        return parts.join(QStringLiteral(", "));
    }
};
