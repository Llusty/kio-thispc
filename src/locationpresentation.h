/*
 * Stateless presentation and basic navigation semantics for browser locations.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QString>
#include <QUrl>
#include <QVector>

struct DriveInfo;

namespace LocationPresentation
{

enum class ParentProfile {
    Primary,
    Split
};

struct Segment
{
    QString text;
    QUrl url;
    QString iconName;
};

QString primaryTitle(const QUrl &url, const QVector<DriveInfo> &drives);
QString splitTitle(const QUrl &url);
QString splitLocationText(const QUrl &url);
QString contentHeaderText(const QUrl &url);
QString iconName(const QUrl &url);

QVector<Segment> localPathSegments(
    const QUrl &url,
    const QVector<DriveInfo> &drives);
QVector<Segment> adminPathSegments(const QUrl &url);
QVector<Segment> remotePathSegments(const QUrl &url);

QUrl parentUrl(
    const QUrl &url,
    const QVector<DriveInfo> &drives,
    ParentProfile profile);

} // namespace LocationPresentation
