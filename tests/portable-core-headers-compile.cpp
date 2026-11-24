/*
 * Dedicated compilation & non-destructive contract verification translation unit
 * for the 8 header-only portable-core modules that do not define Q_OBJECT
 * (0.46.0 Stage 1B-R1 / AUD-1B-02).
 *
 * Ensures all 31 files in `THISPC_PORTABLE_CORE_SOURCES` receive real C++20
 * compilation under both `THISPC_BUILD_PORTABLE_CORE_ONLY=ON` (including
 * Ubuntu 24.04 / Qt 6.4.2) and the KDE compatibility build.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "acldata.h"
#include "aggregatepermissionsmodel.h"
#include "aggregatepermissionswriter.h"
#include "aggregatepropertiesformat.h"
#include "aggregatepropertiesmodel.h"
#include "aggregatepropertiesscan.h"
#include "localfileoverwrite.h"
#include "localmergetransaction.h"

#include <QByteArray>
#include <QString>

#include <type_traits>

bool verifyPortableCoreHeaderOnlyContracts()
{
    static_assert(std::is_default_constructible_v<AclEntryData>);
    static_assert(std::is_default_constructible_v<AggregatePermissionEdit>);
    static_assert(std::is_default_constructible_v<AggregateSummary>);

    bool ok = true;

    // aggregatepropertiesformat.h
    ok = ok && !aggregateCount(42).isEmpty();
    ok = ok && !aggregateSize(1024).isEmpty();

    // localmergetransaction.h (pure validation helpers — zero filesystem I/O)
    ok = ok && LocalMergeTxValidation::isValidSinglePathComponent(QStringLiteral("report.txt"));
    ok = ok && !LocalMergeTxValidation::isValidSinglePathComponent(QStringLiteral(".."));
    ok = ok && !LocalMergeTxValidation::isValidSinglePathComponent(QStringLiteral("a/b"));

    QString decoded;
    ok = ok && LocalMergeTxValidation::decodeStrictBase64Utf8(QByteArray("dGhpc3Bj"), &decoded);
    ok = ok && (decoded == QStringLiteral("thispc"));

    // localfileoverwrite.h (pure string helper — zero filesystem I/O)
    ok = ok && !moveContainmentErrorMessage().isEmpty();

    return ok;
}
