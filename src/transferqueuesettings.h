/*
 * Transfer queue persistence and validation.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "transfertaskdescriptor.h"
#include "transferbandwidthlimiter.h"

#include <QSettings>
#include <QString>
#include <QVariant>
#include <algorithm>

struct TransferQueueConfig
{
    TransferQueueMode mode = TransferQueueMode::Parallel;
    int parallelLimit = TransferQueueDefaultParallelLimit;
    bool bandwidthLimitEnabled = false;
    quint64 bandwidthLimitBytesPerSec = 1024ULL * 1024ULL; // Default 1 MiB/s
    BandwidthUnit bandwidthLimitUnit = BandwidthUnit::MiB_s;
    int bandwidthLimitValue = 1;
    bool closeWindowOnComplete = false;

    bool operator==(const TransferQueueConfig &) const = default;
};

class TransferQueueSettings
{
public:
    static constexpr const char *GroupKey = "TransferQueue";
    static constexpr const char *ModeKey = "ExecutionMode";
    static constexpr const char *ParallelLimitKey = "ParallelLimit";
    static constexpr const char *BandwidthLimitEnabledKey = "BandwidthLimitEnabled";
    static constexpr const char *BandwidthLimitBytesKey = "BandwidthLimitBytes";
    static constexpr const char *BandwidthLimitUnitKey = "BandwidthLimitUnit";
    static constexpr const char *BandwidthLimitValueKey = "BandwidthLimitValue";
    static constexpr const char *CloseWindowOnCompleteKey = "CloseWindowOnComplete";

    static int clampLimit(int limit)
    {
        return std::clamp(limit, TransferQueueMinParallelLimit, TransferQueueMaxParallelLimit);
    }

    static TransferQueueMode parseMode(const QString &raw, TransferQueueMode fallback = TransferQueueMode::Parallel)
    {
        const QString trimmed = raw.trimmed();
        if (trimmed.compare(QStringLiteral("Sequential"), Qt::CaseInsensitive) == 0) {
            return TransferQueueMode::Sequential;
        }
        if (trimmed.compare(QStringLiteral("Parallel"), Qt::CaseInsensitive) == 0) {
            return TransferQueueMode::Parallel;
        }
        return fallback;
    }

    static QString modeToString(TransferQueueMode mode)
    {
        return mode == TransferQueueMode::Sequential ? QStringLiteral("Sequential")
                                                     : QStringLiteral("Parallel");
    }

    static TransferQueueConfig load(QSettings &settings)
    {
        TransferQueueConfig config;
        settings.beginGroup(QLatin1String(GroupKey));

        const QVariant modeVar = settings.value(QLatin1String(ModeKey));
        if (modeVar.isValid()) {
            config.mode = parseMode(modeVar.toString(), TransferQueueMode::Parallel);
        }

        const QVariant limitVar = settings.value(QLatin1String(ParallelLimitKey));
        if (limitVar.isValid()) {
            bool ok = false;
            const int limit = limitVar.toInt(&ok);
            if (ok) {
                config.parallelLimit = clampLimit(limit);
            } else {
                config.parallelLimit = TransferQueueDefaultParallelLimit;
            }
        }

        const QVariant bwEnabledVar = settings.value(QLatin1String(BandwidthLimitEnabledKey));
        if (bwEnabledVar.isValid()) {
            config.bandwidthLimitEnabled = bwEnabledVar.toBool();
        }

        const QVariant bwUnitVar = settings.value(QLatin1String(BandwidthLimitUnitKey));
        if (bwUnitVar.isValid()) {
            config.bandwidthLimitUnit = parseBandwidthUnit(bwUnitVar.toString(), BandwidthUnit::MiB_s);
        }

        const QVariant bwValVar = settings.value(QLatin1String(BandwidthLimitValueKey));
        if (bwValVar.isValid()) {
            bool ok = false;
            const int val = bwValVar.toInt(&ok);
            if (ok && val > 0) {
                config.bandwidthLimitValue = val;
            }
        }

        const QVariant bwBytesVar = settings.value(QLatin1String(BandwidthLimitBytesKey));
        if (bwBytesVar.isValid()) {
            bool ok = false;
            const qulonglong bytes = bwBytesVar.toULongLong(&ok);
            if (ok && bytes > 0) {
                config.bandwidthLimitBytesPerSec = std::clamp<quint64>(
                    bytes, TransferBandwidthLimiter::MinBandwidthLimitBytes, TransferBandwidthLimiter::MaxBandwidthLimitBytes);
                if (!bwValVar.isValid() || !bwUnitVar.isValid()) {
                    config.bandwidthLimitValue = bytesPerSecToUnitValue(config.bandwidthLimitBytesPerSec, config.bandwidthLimitUnit);
                }
            }
        } else if (bwValVar.isValid()) {
            config.bandwidthLimitBytesPerSec = bandwidthToBytesPerSec(config.bandwidthLimitValue, config.bandwidthLimitUnit);
        }

        const QVariant closeVar = settings.value(QLatin1String(CloseWindowOnCompleteKey));
        if (closeVar.isValid()) {
            config.closeWindowOnComplete = closeVar.toBool();
        }

        settings.endGroup();
        return config;
    }

    static void save(QSettings &settings, const TransferQueueConfig &config)
    {
        settings.beginGroup(QLatin1String(GroupKey));
        settings.setValue(QLatin1String(ModeKey), modeToString(config.mode));
        settings.setValue(QLatin1String(ParallelLimitKey), clampLimit(config.parallelLimit));
        settings.setValue(QLatin1String(BandwidthLimitEnabledKey), config.bandwidthLimitEnabled);
        settings.setValue(QLatin1String(BandwidthLimitBytesKey), static_cast<qulonglong>(config.bandwidthLimitBytesPerSec));
        settings.setValue(QLatin1String(BandwidthLimitUnitKey), bandwidthUnitName(config.bandwidthLimitUnit));
        settings.setValue(QLatin1String(BandwidthLimitValueKey), config.bandwidthLimitValue);
        settings.setValue(QLatin1String(CloseWindowOnCompleteKey), config.closeWindowOnComplete);
        settings.endGroup();
    }
};
