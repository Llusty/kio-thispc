/*
 * Neutral pane state shared by the primary and Split View adapters.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QList>
#include <QString>
#include <QUrl>

class QAbstractItemView;

enum class PaneId { Primary, Split };

struct PaneItem {
    QUrl url;
    QString name;
    QString type;
    QString size;
    QString modified;
    bool isDir = false;
};

struct PaneContext {
    PaneId id = PaneId::Primary;
    QUrl directory;
    QAbstractItemView *view = nullptr;
    QList<PaneItem> items;
    bool isDirectory = false;
};
