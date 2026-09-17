/* Archive menu eligibility. Full validation runs asynchronously before extraction.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QUrl>
#include <zlib.h>

// Read at most 64 KiB of compressed input and 512 bytes of output. In particular,
// a gzip stream is not evidence of a tar archive, irrespective of its extension.
inline bool thispcGzipHasTarHeader(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QByteArray input = file.read(65536);
    unsigned char header[512] {};
    z_stream stream {};
    stream.next_in = reinterpret_cast<Bytef *>(input.data());
    stream.avail_in = input.size();
    stream.next_out = header;
    stream.avail_out = sizeof(header);
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return false;
    const int result = inflate(&stream, Z_FINISH);
    const bool complete = stream.avail_out == 0
        && (result == Z_OK || result == Z_STREAM_END || result == Z_BUF_ERROR);
    inflateEnd(&stream);
    if (!complete || header[0] == 0) return false;
    unsigned checksum = 0;
    for (unsigned i = 0; i < sizeof(header); ++i)
        checksum += i >= 148 && i < 156 ? ' ' : header[i];
    bool ok = false;
    const auto recorded = QByteArray(reinterpret_cast<const char *>(header + 148), 8)
        .replace('\0', ' ').trimmed().toUInt(&ok, 8);
    return ok && recorded == checksum;
}

inline bool thispcIsArchiveCandidate(const QUrl &url, bool isDirectory)
{
    if (isDirectory || !url.isLocalFile() || url.hasQuery() || url.hasFragment()) return false;
    const QFileInfo file(url.toLocalFile());
    if (!file.isFile() || !file.isReadable()) return false;
    const QString mime = QMimeDatabase().mimeTypeForFile(
        file.absoluteFilePath(), QMimeDatabase::MatchContent).name();
    if (mime == QStringLiteral("application/gzip")
        || mime == QStringLiteral("application/x-compressed-tar"))
        return thispcGzipHasTarHeader(file.absoluteFilePath());
    return mime == QStringLiteral("application/zip")
        || mime == QStringLiteral("application/x-7z-compressed")
        || mime == QStringLiteral("application/x-tar");
}

inline bool thispcCanExtractArchive(const QUrl &url, bool isDirectory)
{
    return !QStandardPaths::findExecutable(QStringLiteral("ark")).isEmpty()
        && thispcIsArchiveCandidate(url, isDirectory);
}
