/* SPDX-License-Identifier: MIT */
#pragma once
#include <QLocale>

inline QString aggregateCount(quint64 value, const QLocale &locale = QLocale()) {
    auto grouped = locale;
    grouped.setNumberOptions(grouped.numberOptions() & ~QLocale::OmitGroupSeparator);
    QString result = grouped.toString(value);
    // Polish CLDR grouping omits the separator for four-digit integers.
    if (locale.language() == QLocale::Polish && value >= 1000 && value < 10000
        && !result.contains(grouped.groupSeparator()))
        result.insert(1, grouped.groupSeparator());
    return result;
}
inline QString aggregateSize(quint64 bytes, const QLocale &locale = QLocale()) {
    if (bytes < 1024) return aggregateCount(bytes, locale) + QStringLiteral(" B");
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 6) { value /= 1024; ++unit; }
    return QStringLiteral("%1 %2 (%3 %4)")
        .arg(locale.toString(value, 'f', 2), QString::fromLatin1(units[unit]),
             aggregateCount(bytes, locale), locale.language() == QLocale::Polish
                 ? QStringLiteral("bajtów") : QStringLiteral("bytes"));
}
