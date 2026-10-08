// Pure rwx editor policy: canonical 0xxx, preserving known special bits.
#pragma once
#include <QString>
#include "propertiescapabilities.h"
#include <array>
class PropertiesPosixMode {
public:
    static bool editable(const QUrl &url, const PropertiesTargetCapabilities &capabilities) {
        return url.isLocalFile() && capabilities.isLocal && capabilities.entryIdentity.valid
            && (capabilities.entryKind == PropertiesEntryKind::RegularFile
                || capabilities.entryKind == PropertiesEntryKind::Directory)
            && capabilities.posixModeEditable == PropertiesCapabilityState::Supported;
    }
    static bool parse(const QString &text, int *mode) {
        if (text.size() != 4 || text[0] != QLatin1Char('0')) return false;
        int value = 0;
        for (QChar c : text) {
            if (c < QLatin1Char('0') || c > QLatin1Char('7')) return false;
            value = (value << 3) | (c.unicode() - '0');
        }
        if (mode) *mode = value;
        return true;
    }
    static QString format(int mode) {
        return QStringLiteral("0") + QString::number(mode & 0777, 8).rightJustified(3, QLatin1Char('0'));
    }
    static int merge(int original, int rwx) { return (original & 07000) | (rwx & 0777); }
    static std::array<bool, 9> boxes(int mode) {
        std::array<bool, 9> result{};
        for (int i = 0; i < 9; ++i) result[i] = mode & (0400 >> i);
        return result;
    }
    static int fromBoxes(const std::array<bool, 9> &values) {
        int mode = 0;
        for (int i = 0; i < 9; ++i) if (values[i]) mode |= 0400 >> i;
        return mode;
    }
};
