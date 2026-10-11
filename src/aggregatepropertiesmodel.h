/* SPDX-License-Identifier: MIT */
#pragma once
#include <QFile>
#include <QDir>
#include <QList>
#include <QUrl>
#include <sys/stat.h>

struct AggregateRoot {
    QUrl url;
    QByteArray path;
    struct stat identity {};
    bool valid = false;
};
struct AggregateSnapshot {
    QList<AggregateRoot> roots;
    quint64 omitted = 0;
    static AggregateSnapshot capture(const QList<QUrl> &urls) {
        AggregateSnapshot snapshot;
        for (const auto &url : urls) {
            if (snapshot.roots.size() >= 4096) { ++snapshot.omitted; continue; }
            AggregateRoot root;
            root.url = url;
            if (url.isLocalFile()) {
                root.path = QFile::encodeName(QDir::cleanPath(url.toLocalFile()));
                root.valid = ::lstat(root.path.constData(), &root.identity) == 0;
            }
            snapshot.roots.append(root);
        }
        return snapshot;
    }
};
struct AggregateSummary {
    quint64 files = 0, folders = 0, links = 0, bytes = 0;
    quint64 allocatedBytes = 0, allocationUnavailable = 0, allocatedFiles = 0;
    quint64 skipped = 0, unsupported = 0, invalidated = 0;
    bool cancelled = false, limited = false;
    bool partial() const { return allocationUnavailable || skipped || unsupported || invalidated || cancelled || limited; }
};
