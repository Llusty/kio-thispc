/*
 * thispc-view - a lightweight KDE/Qt file browser with a Windows-like
 * "This PC" home page, backed by KIO.
 *
 * Version 0.19.0.3
 * SPDX-License-Identifier: MIT
 */

#include <KIO/CopyJob>
#include <KIO/FileUndoManager>
#include <KIO/Global>
#include <KIO/JobUiDelegateFactory>
#include <KIO/ChmodJob>
#include <KIO/StoredTransferJob>
#include <KIO/ListJob>
#include <KIO/MkdirJob>
#include <KIO/SimpleJob>
#include <KIO/StatJob>
#include <KIO/UDSEntry>
#include <KJob>
#include <KJobUiDelegate>
#include <KFileItem>
#include <KFileItemActions>
#include <KFileItemListProperties>
#include <KProtocolInfo>
#include <KProtocolManager>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFocusEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QMimeData>
#include <QMenu>
#include <QMimeDatabase>
#include <QMimeType>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QPalette>
#include <QPointer>
#include <QProcess>
#include <QPixmap>
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStatusBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTabBar>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidget>
#include <QWidgetAction>

#include <algorithm>
#include <functional>
#include <utility>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{


const QUrl kThisPcUrl(QStringLiteral("thispc:/"));

bool isAdminUrl(const QUrl &url)
{
    return url.scheme() == QStringLiteral("admin");
}

bool adminProtocolAvailable()
{
    return KProtocolInfo::isKnownProtocol(
        QStringLiteral("admin"));
}

QString localPathForFileOrAdmin(const QUrl &url)
{
    if (url.isLocalFile()) {
        return url.toLocalFile();
    }

    if (isAdminUrl(url)) {
        return url.path();
    }

    return {};
}

QUrl adminUrlForLocalPath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }

    QUrl url;
    url.setScheme(QStringLiteral("admin"));
    url.setPath(QDir::cleanPath(path));
    return url;
}

QUrl adminUrlForFileUrl(const QUrl &url)
{
    const QString path =
        localPathForFileOrAdmin(url);

    return adminUrlForLocalPath(path);
}

QUrl ordinaryFileUrlForAdmin(const QUrl &url)
{
    if (!isAdminUrl(url)) {
        return url;
    }

    return QUrl::fromLocalFile(
        QDir::cleanPath(url.path()));
}

QString filesystemTypeForUrl(const QUrl &url)
{
    const QString path =
        localPathForFileOrAdmin(url);

    if (path.isEmpty()) {
        return {};
    }

    QStorageInfo storage(path);
    if (!storage.isValid()) {
        return {};
    }

    return QString::fromLatin1(
        storage.fileSystemType()).toLower();
}

bool filesystemIsReadOnly(const QUrl &url)
{
    const QString path =
        localPathForFileOrAdmin(url);

    if (path.isEmpty()) {
        return false;
    }

    QStorageInfo storage(path);
    return storage.isValid()
        && storage.isReadOnly();
}

bool filesystemMayUseMountControlledPermissions(
    const QString &fileSystem)
{
    const QString fs =
        fileSystem.trimmed().toLower();

    return fs == QStringLiteral("ntfs")
        || fs == QStringLiteral("ntfs3")
        || fs == QStringLiteral("fuseblk")
        || fs == QStringLiteral("exfat")
        || fs == QStringLiteral("vfat")
        || fs == QStringLiteral("fat")
        || fs == QStringLiteral("fat32")
        || fs == QStringLiteral("msdos");
}

bool readKioPermissions(
    const QUrl &url,
    int *mode,
    QString *owner = nullptr,
    QString *group = nullptr,
    QString *error = nullptr)
{
    if (!url.isValid()) {
        if (error) {
            *error = QStringLiteral("Invalid URL");
        }
        return false;
    }

    KIO::StatJob *job =
        KIO::stat(
            url,
            KIO::HideProgressInfo);

    if (!job->exec()) {
        if (error) {
            *error = job->errorString();
        }
        return false;
    }

    const KIO::UDSEntry entry =
        job->statResult();

    const qlonglong access =
        entry.numberValue(
            KIO::UDSEntry::UDS_ACCESS,
            -1);

    if (access < 0) {
        if (error) {
            *error = QStringLiteral(
                "Permission bits were not returned by the KIO worker.");
        }
        return false;
    }

    if (mode) {
        *mode = static_cast<int>(access) & 0777;
    }

    if (owner) {
        *owner =
            entry.stringValue(
                KIO::UDSEntry::UDS_USER);
    }

    if (group) {
        *group =
            entry.stringValue(
                KIO::UDSEntry::UDS_GROUP);
    }

    return true;
}

bool readKioFileItem(
    const QUrl &url,
    KFileItem *item,
    QString *error = nullptr)
{
    if (!url.isValid() || !item) {
        if (error) {
            *error = QStringLiteral("Invalid URL");
        }
        return false;
    }

    KIO::StatJob *job =
        KIO::stat(
            url,
            KIO::HideProgressInfo);

    if (!job->exec()) {
        if (error) {
            *error = job->errorString();
        }
        return false;
    }

    *item = KFileItem(
        job->statResult(),
        url,
        true,
        false);
    return true;
}

bool localEntryOwnedByCurrentUser(
    const QUrl &url)
{
    if (!url.isLocalFile()) {
        return false;
    }

#ifdef Q_OS_UNIX
    const QFileInfo info(url.toLocalFile());
    return info.exists()
        && (geteuid() == 0
            || info.ownerId()
                == static_cast<uint>(geteuid()));
#else
    return QFileInfo(url.toLocalFile()).isWritable();
#endif
}

struct DriveInfo
{
    QString name;
    QString freeText;
    QString capacityText;
    QString usedText;
    QString fileSystem;
    QString mountPoint;
    QUrl targetUrl;
    QString iconName;
    int usedPercent = 0;
};

struct FileInfo
{
    QString name;
    QString mimeType;
    QString iconName;
    QUrl url;
    bool isDir = false;
    qint64 size = -1;
    qint64 modificationTime = 0;
};

bool isPolish()
{
    return QLocale().language() == QLocale::Polish;
}

QString trLocal(const char *polish, const char *english)
{
    return isPolish()
        ? QString::fromUtf8(polish)
        : QString::fromUtf8(english);
}

QString formatFileSize(qint64 bytes, bool isDirectory)
{
    if (isDirectory || bytes < 0) {
        return QStringLiteral("—");
    }

    static const char *units[] = {
        "B", "KiB", "MiB", "GiB", "TiB", "PiB"
    };

    double value = static_cast<double>(bytes);
    int unit = 0;

    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }

    int precision = 0;
    if (unit > 0 && value < 10.0) {
        precision = 2;
    } else if (unit > 0 && value < 100.0) {
        precision = 1;
    }

    return QStringLiteral("%1 %2")
        .arg(
            QLocale().toString(value, 'f', precision),
            QString::fromLatin1(units[unit]));
}

QString formatModificationTime(qint64 seconds)
{
    if (seconds <= 0) {
        return QStringLiteral("—");
    }

    const QDateTime dateTime =
        QDateTime::fromSecsSinceEpoch(seconds);

    return QLocale().toString(
        dateTime,
        QLocale::ShortFormat);
}

QMimeType resolvedMimeType(
    const FileInfo &file,
    QMimeDatabase &mimeDatabase)
{
    if (file.isDir) {
        return mimeDatabase.mimeTypeForName(
            QStringLiteral("inode/directory"));
    }

    if (!file.mimeType.isEmpty()
        && file.mimeType != QStringLiteral("application/octet-stream")) {
        const QMimeType known =
            mimeDatabase.mimeTypeForName(file.mimeType);
        if (known.isValid() && !known.isDefault()) {
            return known;
        }
    }

    // Some KIO workers/filesystems do not provide UDS_MIME_TYPE. Resolve by
    // extension first; this is enough to get PNG/JPEG/PDF/archive/etc icons
    // without synchronously reading file contents.
    const QString probe =
        file.url.isLocalFile()
            ? file.url.toLocalFile()
            : file.name;

    QMimeType mime =
        mimeDatabase.mimeTypeForFile(
            probe,
            QMimeDatabase::MatchExtension);

    if (!mime.isValid() || mime.isDefault()) {
        mime = mimeDatabase.mimeTypeForFile(
            file.name,
            QMimeDatabase::MatchExtension);
    }

    return mime;
}

QString fileTypeLabel(
    const FileInfo &file,
    QMimeDatabase &mimeDatabase)
{
    if (file.isDir) {
        return trLocal("Folder", "Folder");
    }

    const QMimeType mime =
        resolvedMimeType(file, mimeDatabase);

    if (mime.isValid() && !mime.isDefault()) {
        if (!mime.comment().isEmpty()) {
            return mime.comment();
        }
        return mime.name();
    }

    return trLocal("Plik", "File");
}

QUrl normalizedUrl(QUrl url)
{
    if (url.isLocalFile()) {
        url = QUrl::fromLocalFile(QDir::cleanPath(url.toLocalFile()));
        return url;
    }

    QString path = url.path();
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    url.setPath(path);
    return url;
}

bool sameLocation(const QUrl &a, const QUrl &b)
{
    return normalizedUrl(a) == normalizedUrl(b);
}

bool isWithinLocation(const QUrl &childRaw, const QUrl &baseRaw)
{
    const QUrl child = normalizedUrl(childRaw);
    const QUrl base = normalizedUrl(baseRaw);

    if (!child.isValid() || !base.isValid()) {
        return false;
    }

    if (sameLocation(child, base)) {
        return true;
    }

    if (child.scheme() != base.scheme()) {
        return false;
    }

    if (child.isLocalFile() && base.isLocalFile()) {
        const QString childPath = QDir::cleanPath(child.toLocalFile());
        const QString basePath = QDir::cleanPath(base.toLocalFile());

        if (basePath == QStringLiteral("/")) {
            return childPath.startsWith(QLatin1Char('/'));
        }

        return childPath.startsWith(basePath + QDir::separator());
    }

    QString childPath = child.path();
    QString basePath = base.path();

    if (!basePath.endsWith(QLatin1Char('/'))) {
        basePath += QLatin1Char('/');
    }

    return childPath.startsWith(basePath);
}

Qt::DropAction dropActionForUrls(
    const QList<QUrl> &urls,
    const QUrl &destination,
    Qt::KeyboardModifiers modifiers);

bool dropWouldCreateCycle(
    const QList<QUrl> &urls,
    const QUrl &destination);

int locationDepth(const QUrl &url)
{
    if (url.isLocalFile()) {
        return QDir::cleanPath(url.toLocalFile())
            .split(QDir::separator(), Qt::SkipEmptyParts)
            .size();
    }

    return url.path()
        .split(QLatin1Char('/'), Qt::SkipEmptyParts)
        .size();
}

namespace {

QString existingPathForStorage(const QString &rawPath)
{
    QFileInfo info(rawPath);
    if (info.exists()) {
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) {
            return canonical;
        }
    }

    QDir parent = info.dir();
    while (!parent.exists() && parent.cdUp()) {
    }
    const QString canonicalParent = parent.exists()
        ? QFileInfo(parent.absolutePath()).canonicalFilePath()
        : QString();
    return canonicalParent.isEmpty()
        ? QDir::cleanPath(info.absolutePath())
        : QDir(canonicalParent).filePath(info.fileName());
}

bool localPathsShareStorage(
    const QString &sourcePath,
    const QString &destinationPath)
{
    const QString sourceStoragePath = existingPathForStorage(sourcePath);
    const QString destinationStoragePath = existingPathForStorage(destinationPath);
    QStorageInfo sourceStorage(sourceStoragePath);
    QStorageInfo destinationStorage(destinationStoragePath);
    if (!sourceStorage.isValid() || !destinationStorage.isValid()) {
        return false;
    }

    const QString sourceRoot =
        QDir::cleanPath(sourceStorage.rootPath());
    const QString destinationRoot =
        QDir::cleanPath(destinationStorage.rootPath());

    if (!sourceStorage.device().isEmpty()
        && !destinationStorage.device().isEmpty()
        && sourceStorage.device() != destinationStorage.device()) {
        return false;
    }

    // Be conservative around bind mounts/subvolumes: an identical device
    // name alone does not guarantee that a rename-style move can cross
    // the two mounted roots. Copy is the safe default in that case.
    return sourceRoot == destinationRoot;
}

bool localPathIsWithin(
    const QString &childPath,
    const QString &basePath)
{
    const QString child = QFileInfo(childPath).canonicalFilePath();
    const QString base = QFileInfo(basePath).canonicalFilePath();
    if (child.isEmpty() || base.isEmpty()) {
        return false;
    }
    const QString relative = QDir(base).relativeFilePath(child);
    return relative.isEmpty()
        || (relative != QStringLiteral("..")
            && !relative.startsWith(QStringLiteral("..%1")
                .arg(QDir::separator()))
            && !QDir::isAbsolutePath(relative));
}

}

Qt::DropAction dropActionForUrls(
    const QList<QUrl> &urls,
    const QUrl &destination,
    Qt::KeyboardModifiers modifiers)
{
    if (modifiers.testFlag(Qt::ControlModifier)) {
        return Qt::CopyAction;
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        return Qt::MoveAction;
    }

    if (!destination.isLocalFile() || urls.isEmpty()) {
        return Qt::CopyAction;
    }
    const QString destinationPath = destination.toLocalFile();
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()
            || !localPathsShareStorage(url.toLocalFile(), destinationPath)) {
            return Qt::CopyAction;
        }
    }
    return Qt::MoveAction;
}

bool dropWouldCreateCycle(
    const QList<QUrl> &urls,
    const QUrl &destination)
{
    for (const QUrl &url : urls) {
        if (url.isLocalFile() && destination.isLocalFile()) {
            const QFileInfo sourceInfo(url.toLocalFile());
            if (sourceInfo.isDir()
                && localPathIsWithin(
                    destination.toLocalFile(),
                    sourceInfo.absoluteFilePath())) {
                return true;
            }
        } else if (sameLocation(url, destination)
                   || isWithinLocation(destination, url)) {
            return true;
        }
    }
    return false;
}

QString urlForDisplay(const QUrl &url)
{
    if (sameLocation(url, kThisPcUrl)) {
        return QStringLiteral("thispc:/");
    }

    if (url.isLocalFile()) {
        return url.toLocalFile();
    }

    return url.toDisplayString(QUrl::PreferLocalFile);
}

QString parentLocationForDisplay(const QUrl &url)
{
    if (url.isLocalFile()) {
        return QFileInfo(url.toLocalFile()).absolutePath();
    }

    QUrl parent = url;
    QString path = parent.path();

    while (path.size() > 1
           && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }

    const int slash =
        path.lastIndexOf(QLatin1Char('/'));

    if (slash <= 0) {
        path = QStringLiteral("/");
    } else {
        path = path.left(slash);
    }

    parent.setPath(path);
    parent.setQuery(QString());
    return parent.toDisplayString(QUrl::PreferLocalFile);
}

QUrl urlFromUserText(const QString &text)
{
    const QString trimmed = text.trimmed();

    if (trimmed.isEmpty()) {
        return {};
    }

    if (trimmed.startsWith(QLatin1Char('/'))) {
        return QUrl::fromLocalFile(trimmed);
    }

    if (trimmed.contains(QStringLiteral(":/"))) {
        return QUrl(trimmed);
    }

    return QUrl::fromUserInput(trimmed);
}

bool isSearchLocation(const QUrl &url)
{
    return url.scheme() == QStringLiteral("thispcsearch");
}

QUrl filenameSearchUrl(
    const QString &query,
    const QUrl &root)
{
    QUrl searchUrl(QStringLiteral("filenamesearch:"));

    QUrlQuery parameters;
    parameters.addQueryItem(
        QStringLiteral("search"),
        query);
    parameters.addQueryItem(
        QStringLiteral("url"),
        root.toString());

    searchUrl.setQuery(parameters);
    return searchUrl;
}

QUrl makeSearchLocation(
    const QString &query,
    int scope,
    const QUrl &base,
    int typeFilter,
    int dateFilter,
    int sizeFilter)
{
    QUrl searchUrl(QStringLiteral("thispcsearch:/"));

    QUrlQuery parameters;
    parameters.addQueryItem(
        QStringLiteral("search"),
        query);
    parameters.addQueryItem(
        QStringLiteral("scope"),
        QString::number(scope));

    if (base.isValid()) {
        parameters.addQueryItem(
            QStringLiteral("base"),
            base.toString());
    }

    parameters.addQueryItem(
        QStringLiteral("type"),
        QString::number(typeFilter));
    parameters.addQueryItem(
        QStringLiteral("date"),
        QString::number(dateFilter));
    parameters.addQueryItem(
        QStringLiteral("size"),
        QString::number(sizeFilter));

    searchUrl.setQuery(parameters);
    return searchUrl;
}

QString searchQueryFromUrl(const QUrl &url)
{
    if (!isSearchLocation(url)
        && url.scheme() != QStringLiteral("filenamesearch")) {
        return {};
    }

    return QUrlQuery(url).queryItemValue(
        QStringLiteral("search"));
}

int searchIntParameter(
    const QUrl &url,
    const QString &name,
    int fallback)
{
    bool ok = false;
    const int value =
        QUrlQuery(url).queryItemValue(name).toInt(&ok);

    return ok ? value : fallback;
}

QUrl searchBaseFromUrl(const QUrl &url)
{
    const QString base =
        QUrlQuery(url).queryItemValue(
            QStringLiteral("base"));

    return base.isEmpty()
        ? QUrl()
        : QUrl(base);
}

void openInDolphin(const QUrl &url)
{
    if (!url.isValid()) {
        return;
    }

    const QString argument =
        url.isLocalFile() ? url.toLocalFile() : url.toString();

    if (!QProcess::startDetached(QStringLiteral("dolphin"), {argument})) {
        QDesktopServices::openUrl(url);
    }
}

bool openTerminalAt(const QUrl &url)
{
    if (!url.isLocalFile()) {
        return false;
    }

    const QString path = url.toLocalFile();

    if (QProcess::startDetached(
            QStringLiteral("konsole"),
            {QStringLiteral("--workdir"), path})) {
        return true;
    }

    return QProcess::startDetached(
        QStringLiteral("x-terminal-emulator"),
        {},
        path);
}

void makePassive(QWidget *widget)
{
    widget->setAttribute(Qt::WA_TransparentForMouseEvents);
}

QIcon themedIcon(const QString &preferred,
                 const QString &fallback = QStringLiteral("folder"))
{
    QIcon icon = QIcon::fromTheme(preferred);
    if (icon.isNull()) {
        icon = QIcon::fromTheme(fallback);
    }
    return icon;
}

QString driveTooltip(const DriveInfo &drive)
{
    return drive.name
        + QStringLiteral("\n")
        + drive.freeText
        + trLocal(" wolne z ", " free of ")
        + drive.capacityText
        + QStringLiteral("\n")
        + trLocal("System plików: ", "Filesystem: ")
        + drive.fileSystem
        + QStringLiteral("\n")
        + trLocal("Punkt montowania: ", "Mount point: ")
        + drive.mountPoint;
}

void populateDriveContextMenu(QMenu &menu, const DriveInfo &drive)
{
    QAction *openAction = menu.addAction(
        themedIcon(QStringLiteral("system-file-manager")),
        trLocal("Otwórz w Dolphinie", "Open in Dolphin"));

    QAction *copyPathAction = menu.addAction(
        themedIcon(QStringLiteral("edit-copy")),
        trLocal("Kopiuj punkt montowania", "Copy mount point"));

    QAction *chosen = menu.exec(QCursor::pos());

    if (chosen == openAction) {
        openInDolphin(drive.targetUrl);
    } else if (chosen == copyPathAction) {
        QGuiApplication::clipboard()->setText(drive.mountPoint);
    }
}

QUrl childUrlForEntry(const QUrl &base, const KIO::UDSEntry &entry)
{
    const QString target =
        entry.stringValue(KIO::UDSEntry::UDS_TARGET_URL);
    if (!target.isEmpty()) {
        return QUrl(target);
    }

    const QString explicitUrl =
        entry.stringValue(KIO::UDSEntry::UDS_URL);
    if (!explicitUrl.isEmpty()) {
        return QUrl(explicitUrl);
    }

    QUrl child = base;
    QString path = child.path();
    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }

    path += entry.stringValue(KIO::UDSEntry::UDS_NAME);
    child.setPath(path);
    return child;
}

QUrl childUrlWithName(const QUrl &directory, const QString &name)
{
    QUrl child = directory;
    QString path = child.path();

    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }

    path += name;
    child.setPath(path);
    return child;
}

QUrl siblingUrlWithName(const QUrl &source, const QString &name)
{
    QUrl destination = source;
    QString path = destination.path();

    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }

    const int slash = path.lastIndexOf(QLatin1Char('/'));

    QString parentPath;
    if (slash < 0) {
        parentPath = QStringLiteral("/");
    } else {
        parentPath = path.left(slash + 1);
    }

    destination.setPath(parentPath + name);
    return destination;
}

bool validNewName(const QString &name)
{
    const QString trimmed = name.trimmed();

    return !trimmed.isEmpty()
        && trimmed != QStringLiteral(".")
        && trimmed != QStringLiteral("..")
        && !trimmed.contains(QLatin1Char('/'));
}

bool entryIsDirectory(const KIO::UDSEntry &entry)
{
    const QString mime =
        entry.stringValue(KIO::UDSEntry::UDS_MIME_TYPE);

    if (mime == QStringLiteral("inode/directory")) {
        return true;
    }

#ifndef Q_OS_WIN
    const long long type =
        entry.numberValue(KIO::UDSEntry::UDS_FILE_TYPE, 0);
    if (type != 0 && S_ISDIR(static_cast<mode_t>(type))) {
        return true;
    }
#endif

    return false;
}

class AddressLineEdit : public QLineEdit
{
    Q_OBJECT

public:
    explicit AddressLineEdit(QWidget *parent = nullptr)
        : QLineEdit(parent)
    {
    }

Q_SIGNALS:
    void canceled();
    void focusLeft();

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            Q_EMIT canceled();
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QLineEdit::focusOutEvent(event);
        Q_EMIT focusLeft();
    }
};

class BreadcrumbFrame : public QFrame
{
    Q_OBJECT

public:
    explicit BreadcrumbFrame(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setCursor(Qt::IBeamCursor);
    }

Q_SIGNALS:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            Q_EMIT clicked();
        }

        QFrame::mousePressEvent(event);
    }
};

class ClickableFrame : public QFrame
{
    Q_OBJECT

public:
    explicit ClickableFrame(const QUrl &url, QWidget *parent = nullptr)
        : QFrame(parent)
        , m_url(url)
    {
        setObjectName(QStringLiteral("thispcCard"));
        setFrameShape(QFrame::NoFrame);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover, true);
    }

Q_SIGNALS:
    void activated(const QUrl &url);

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            setFocus(Qt::MouseFocusReason);
        }
        QFrame::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            Q_EMIT activated(m_url);
            event->accept();
            return;
        }
        QFrame::mouseDoubleClickEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter) {
            Q_EMIT activated(m_url);
            event->accept();
            return;
        }
        QFrame::keyPressEvent(event);
    }

protected:
    QUrl targetUrl() const
    {
        return m_url;
    }

private:
    QUrl m_url;
};

class DriveFrame : public ClickableFrame
{
    Q_OBJECT

public:
    explicit DriveFrame(const DriveInfo &drive, QWidget *parent = nullptr)
        : ClickableFrame(drive.targetUrl, parent)
        , m_drive(drive)
    {
    }

protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        QMenu menu(this);
        populateDriveContextMenu(menu, m_drive);
        event->accept();
    }

private:
    DriveInfo m_drive;
};

class SidebarButton : public QPushButton
{
    Q_OBJECT

public:
    SidebarButton(const QString &text,
                  const QString &iconName,
                  const QUrl &url,
                  QWidget *parent = nullptr)
        : QPushButton(themedIcon(iconName), text, parent)
        , m_url(url)
    {
        setObjectName(QStringLiteral("sidebarButton"));
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        setIconSize(QSize(18, 18));
        setMinimumHeight(31);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        connect(this, &QPushButton::clicked, this, [this] {
            Q_EMIT activated(m_url);
        });
    }

    QUrl url() const
    {
        return m_url;
    }

    void setCurrent(bool current)
    {
        setProperty("current", current);
        style()->unpolish(this);
        style()->polish(this);
        update();
    }

Q_SIGNALS:
    void activated(const QUrl &url);
    void openInNewTabRequested(const QUrl &url, bool makeCurrent);
    void openInNewWindowRequested(const QUrl &url);
    void openInSplitPaneRequested(const QUrl &url);

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton) {
            // Browser/file-manager convention: middle click opens a
            // background tab and keeps the current tab selected.
            Q_EMIT openInNewTabRequested(m_url, false);
            event->accept();
            return;
        }

        QPushButton::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        QMenu menu(this);

        QAction *openAction = menu.addAction(
            themedIcon(QStringLiteral("folder-open")),
            trLocal("Otwórz", "Open"));

        QAction *newTabAction = menu.addAction(
            themedIcon(QStringLiteral("tab-new")),
            trLocal(
                "Otwórz w nowej karcie",
                "Open in new tab"));

        QAction *newWindowAction = menu.addAction(
            themedIcon(QStringLiteral("window-new")),
            trLocal(
                "Otwórz w nowym oknie",
                "Open in new window"));

        QAction *splitPaneAction = menu.addAction(
            themedIcon(QStringLiteral("view-split-left-right"), QStringLiteral("view-list-details")),
            trLocal(
                "Otwórz w drugim panelu",
                "Open in other pane"));

        QAction *chosen = menu.exec(event->globalPos());

        if (chosen == openAction) {
            Q_EMIT activated(m_url);
        } else if (chosen == newTabAction) {
            Q_EMIT openInNewTabRequested(m_url, true);
        } else if (chosen == newWindowAction) {
            Q_EMIT openInNewWindowRequested(m_url);
        } else if (chosen == splitPaneAction) {
            Q_EMIT openInSplitPaneRequested(m_url);
        }

        event->accept();
    }

private:
    QUrl m_url;
};

class SidebarDriveButton : public QFrame
{
    Q_OBJECT

public:
    SidebarDriveButton(const DriveInfo &drive, QWidget *parent = nullptr)
        : QFrame(parent)
        , m_drive(drive)
    {
        setObjectName(QStringLiteral("sidebarDrive"));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip(driveTooltip(drive));

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 4, 8, 4);
        layout->setSpacing(7);

        QString iconName = QStringLiteral("drive-harddisk");
        if (drive.iconName.contains(QStringLiteral("removable"))) {
            iconName = QStringLiteral("drive-removable-media");
        }

        auto *icon = new QLabel(this);
        icon->setPixmap(
            themedIcon(iconName, QStringLiteral("drive-harddisk")).pixmap(17, 17));
        icon->setFixedSize(19, 19);
        makePassive(icon);

        auto *body = new QVBoxLayout;
        body->setSpacing(2);
        body->setContentsMargins(0, 0, 0, 0);

        auto *name = new QLabel(drive.name, this);
        makePassive(name);

        auto *bar = new QProgressBar(this);
        bar->setObjectName(QStringLiteral("sidebarProgress"));
        bar->setRange(0, 100);
        bar->setValue(drive.usedPercent);
        bar->setTextVisible(false);
        bar->setFixedHeight(5);
        makePassive(bar);

        body->addWidget(name);
        body->addWidget(bar);

        layout->addWidget(icon, 0, Qt::AlignVCenter);
        layout->addLayout(body, 1);
    }

    QUrl url() const
    {
        return m_drive.targetUrl;
    }

    void setCurrent(bool current)
    {
        setProperty("current", current);
        style()->unpolish(this);
        style()->polish(this);
        update();
    }

Q_SIGNALS:
    void activated(const QUrl &url);
    void openInNewTabRequested(const QUrl &url, bool makeCurrent);
    void openInNewWindowRequested(const QUrl &url);
    void openInSplitPaneRequested(const QUrl &url);

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            setFocus(Qt::MouseFocusReason);
            Q_EMIT activated(m_drive.targetUrl);
            event->accept();
            return;
        }

        if (event->button() == Qt::MiddleButton) {
            Q_EMIT openInNewTabRequested(
                m_drive.targetUrl,
                false);
            event->accept();
            return;
        }

        QFrame::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter) {
            Q_EMIT activated(m_drive.targetUrl);
            event->accept();
            return;
        }
        QFrame::keyPressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        QMenu menu(this);

        QAction *openAction = menu.addAction(
            themedIcon(QStringLiteral("folder-open")),
            trLocal("Otwórz", "Open"));

        QAction *newTabAction = menu.addAction(
            themedIcon(QStringLiteral("tab-new")),
            trLocal(
                "Otwórz w nowej karcie",
                "Open in new tab"));

        QAction *newWindowAction = menu.addAction(
            themedIcon(QStringLiteral("window-new")),
            trLocal(
                "Otwórz w nowym oknie",
                "Open in new window"));

        QAction *splitPaneAction = menu.addAction(
            themedIcon(QStringLiteral("view-split-left-right"), QStringLiteral("view-list-details")),
            trLocal(
                "Otwórz w drugim panelu",
                "Open in other pane"));

        menu.addSeparator();

        QAction *openDolphinAction = menu.addAction(
            themedIcon(QStringLiteral("system-file-manager")),
            trLocal(
                "Otwórz w Dolphinie",
                "Open in Dolphin"));

        QAction *copyPathAction = menu.addAction(
            themedIcon(QStringLiteral("edit-copy")),
            trLocal(
                "Kopiuj punkt montowania",
                "Copy mount point"));

        QAction *chosen = menu.exec(event->globalPos());

        if (chosen == openAction) {
            Q_EMIT activated(m_drive.targetUrl);
        } else if (chosen == newTabAction) {
            Q_EMIT openInNewTabRequested(
                m_drive.targetUrl,
                true);
        } else if (chosen == newWindowAction) {
            Q_EMIT openInNewWindowRequested(
                m_drive.targetUrl);
        } else if (chosen == splitPaneAction) {
            Q_EMIT openInSplitPaneRequested(
                m_drive.targetUrl);
        } else if (chosen == openDolphinAction) {
            openInDolphin(m_drive.targetUrl);
        } else if (chosen == copyPathAction) {
            QGuiApplication::clipboard()->setText(
                m_drive.mountPoint);
        }

        event->accept();
    }

private:
    DriveInfo m_drive;
};

class CollapsibleSection : public QWidget
{
    Q_OBJECT

public:
    CollapsibleSection(const QString &title,
                       const QString &settingsKey,
                       QWidget *parent = nullptr)
        : QWidget(parent)
        , m_settingsKey(settingsKey)
    {
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 2, 0, 3);
        root->setSpacing(1);

        m_header = new QToolButton(this);
        m_header->setObjectName(QStringLiteral("sidebarSectionButton"));
        m_header->setText(title);
        m_header->setCheckable(true);
        m_header->setChecked(true);
        m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_header->setArrowType(Qt::DownArrow);
        m_header->setIconSize(QSize(16, 16));
        m_header->setMinimumHeight(28);
        m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_content = new QWidget(this);
        m_contentLayout = new QVBoxLayout(m_content);
        m_contentLayout->setContentsMargins(0, 0, 0, 0);
        m_contentLayout->setSpacing(1);

        root->addWidget(m_header);
        root->addWidget(m_content);

        QSettings settings;
        const bool expanded =
            settings.value(
                QStringLiteral("sidebar/") + settingsKey,
                true).toBool();

        setExpanded(expanded);

        connect(m_header, &QToolButton::toggled, this, [this](bool checked) {
            setExpanded(checked);

            QSettings settings;
            settings.setValue(
                QStringLiteral("sidebar/") + m_settingsKey,
                checked);
        });
    }

    QVBoxLayout *contentLayout() const
    {
        return m_contentLayout;
    }

    void setExpanded(bool expanded)
    {
        m_header->blockSignals(true);
        m_header->setChecked(expanded);
        m_header->setArrowType(
            expanded ? Qt::DownArrow : Qt::RightArrow);
        m_content->setVisible(expanded);
        m_header->blockSignals(false);
    }

private:
    QString m_settingsKey;
    QToolButton *m_header = nullptr;
    QWidget *m_content = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;
};

ClickableFrame *makeFolderCard(const QString &name,
                               const QString &path,
                               const QString &iconName,
                               QWidget *parent)
{
    const QUrl url = QUrl::fromLocalFile(path);

    auto *card = new ClickableFrame(url, parent);
    card->setMinimumHeight(64);
    card->setMaximumHeight(68);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    card->setToolTip(path);
    card->setAccessibleName(name);

    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(11);

    auto *icon = new QLabel(card);
    icon->setPixmap(themedIcon(iconName).pixmap(36, 36));
    icon->setFixedSize(40, 40);
    icon->setAlignment(Qt::AlignCenter);
    makePassive(icon);

    auto *text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(1);

    auto *title = new QLabel(name, card);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    makePassive(title);

    auto *subtitle =
        new QLabel(trLocal("Folder użytkownika", "User folder"), card);
    subtitle->setForegroundRole(QPalette::PlaceholderText);
    makePassive(subtitle);

    text->addWidget(title);
    text->addWidget(subtitle);

    layout->addWidget(icon, 0, Qt::AlignVCenter);
    layout->addLayout(text, 1);

    return card;
}

DriveFrame *makeDriveCard(const DriveInfo &drive, QWidget *parent)
{
    auto *card = new DriveFrame(drive, parent);
    card->setMinimumHeight(88);
    card->setMaximumHeight(94);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    card->setAccessibleName(drive.name);
    card->setToolTip(driveTooltip(drive));

    auto *outer = new QHBoxLayout(card);
    outer->setContentsMargins(13, 9, 13, 9);
    outer->setSpacing(12);

    auto *icon = new QLabel(card);

    QString themeIcon = QStringLiteral("drive-harddisk");
    if (drive.iconName.contains(QStringLiteral("removable"))) {
        themeIcon = QStringLiteral("drive-removable-media");
    }

    icon->setPixmap(
        themedIcon(themeIcon, QStringLiteral("drive-harddisk")).pixmap(44, 44));
    icon->setFixedSize(48, 48);
    icon->setAlignment(Qt::AlignCenter);
    makePassive(icon);

    auto *body = new QVBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(4);

    auto *title = new QLabel(drive.name, card);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    makePassive(title);

    auto *progress = new QProgressBar(card);
    progress->setObjectName(QStringLiteral("driveProgress"));
    progress->setRange(0, 100);
    progress->setValue(drive.usedPercent);
    progress->setTextVisible(false);
    progress->setFixedHeight(8);
    progress->setMaximumWidth(335);
    makePassive(progress);

    const QString capacity =
        drive.freeText
        + trLocal(" wolne z ", " free of ")
        + drive.capacityText;

    auto *subtitle = new QLabel(capacity, card);
    subtitle->setForegroundRole(QPalette::PlaceholderText);
    makePassive(subtitle);

    body->addWidget(title);
    body->addWidget(progress);
    body->addWidget(subtitle);

    outer->addWidget(icon, 0, Qt::AlignVCenter);
    outer->addLayout(body, 1);

    return card;
}

void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }

        if (QLayout *child = item->layout()) {
            clearLayout(child);
            delete child;
        }

        delete item;
    }
}


// QListView::setMovement() and setViewMode() reset drag/drop settings.
// Apply this after layout configuration, keeping Static item positioning.
void configureDirectoryDragDrop(QAbstractItemView *view)
{
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->setDragEnabled(true);
    view->setAcceptDrops(true);
    view->viewport()->setAcceptDrops(true);
    view->setDropIndicatorShown(true);
    view->setDefaultDropAction(Qt::CopyAction);
}

class ExplorerNameDelegate : public QStyledItemDelegate
{
public:
    explicit ExplorerNameDelegate(QListView *view)
        : QStyledItemDelegate(view)
        , m_view(view)
    {
    }

    void setAlwaysShowFullNames(bool enabled)
    {
        if (m_alwaysShowFullNames == enabled) {
            return;
        }
        m_alwaysShowFullNames = enabled;
        if (m_view) {
            m_view->doItemsLayout();
            m_view->viewport()->update();
        }
    }

    bool alwaysShowFullNames() const
    {
        return m_alwaysShowFullNames;
    }

    void paint(
        QPainter *painter,
        const QStyleOptionViewItem &option,
        const QModelIndex &index) const override
    {
        QStyleOptionViewItem adjusted(option);
        initStyleOption(&adjusted, index);

        // Keep the normal grid geometry stable.  Selection no longer changes
        // an item's size: a separate overlay owned by DirectoryListWidget
        // displays the complete name of the current selected item.
        if (m_alwaysShowFullNames) {
            adjusted.textElideMode = Qt::ElideNone;
            adjusted.features.setFlag(QStyleOptionViewItem::WrapText, true);
        } else {
            adjusted.textElideMode = Qt::ElideRight;
            adjusted.features.setFlag(QStyleOptionViewItem::WrapText, false);
        }

        const QWidget *widget = adjusted.widget;
        QStyle *style = widget
            ? widget->style()
            : QApplication::style();
        style->drawControl(
            QStyle::CE_ItemViewItem,
            &adjusted,
            painter,
            widget);
    }

    QSize sizeHint(
        const QStyleOptionViewItem &option,
        const QModelIndex &index) const override
    {
        QStyleOptionViewItem adjusted(option);
        initStyleOption(&adjusted, index);

        if (!m_view) {
            return QStyledItemDelegate::sizeHint(adjusted, index);
        }

        const QFontMetrics metrics(adjusted.font);
        constexpr int fullNameLines = 3;

        if (m_view->viewMode() == QListView::ListMode) {
            const int width = qMax(180, m_view->viewport()->width() - 16);
            const int compactHeight = qMax(36, metrics.lineSpacing() + 14);
            const int fullHeight = qMax(
                compactHeight,
                fullNameLines * metrics.lineSpacing() + 12);
            return QSize(
                width,
                m_alwaysShowFullNames ? fullHeight : compactHeight);
        }

        constexpr int itemWidth = 136;
        constexpr int compactHeight = 108;
        const int fullHeight = qMax(
            compactHeight,
            64 + 10 + fullNameLines * metrics.lineSpacing() + 14);
        return QSize(
            itemWidth,
            m_alwaysShowFullNames ? fullHeight : compactHeight);
    }

private:
    QListView *m_view = nullptr;
    bool m_alwaysShowFullNames = false;
};

class DirectoryListWidget : public QListWidget
{
    Q_OBJECT

public:
    explicit DirectoryListWidget(QWidget *parent = nullptr)
        : QListWidget(parent)
    {
        m_nameDelegate = new ExplorerNameDelegate(this);
        setItemDelegate(m_nameDelegate);
        setTextElideMode(Qt::ElideRight);
        setWordWrap(true);

        m_nameOverlay = new QLabel(viewport());
        m_nameOverlay->setObjectName(QStringLiteral("selectedNameOverlay"));
        m_nameOverlay->setWordWrap(true);
        m_nameOverlay->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        m_nameOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_nameOverlay->setTextInteractionFlags(Qt::NoTextInteraction);
        m_nameOverlay->setMargin(4);
        m_nameOverlay->setStyleSheet(QStringLiteral(
            "QLabel#selectedNameOverlay {"
            " background: palette(base);"
            " color: palette(text);"
            " border: 1px solid palette(highlight);"
            " border-radius: 4px;"
            " }"));
        m_nameOverlay->hide();

        connect(
            this,
            &QListWidget::itemSelectionChanged,
            this,
            [this] {
                viewport()->update();
                updateNameOverlay();
            });
        connect(
            this,
            &QListWidget::currentItemChanged,
            this,
            [this] {
                updateNameOverlay();
            });
    }

    void setAlwaysShowFullNames(bool enabled)
    {
        if (m_nameDelegate) {
            m_nameDelegate->setAlwaysShowFullNames(enabled);
        }
        QTimer::singleShot(0, this, [this] {
            updateNameOverlay();
        });
    }

    bool alwaysShowFullNames() const
    {
        return m_nameDelegate
            && m_nameDelegate->alwaysShowFullNames();
    }

    void setDropDirectory(const QUrl &url)
    {
        m_dropDirectory = url;
    }

Q_SIGNALS:
    void urlsDropped(
        const QList<QUrl> &urls,
        const QUrl &destination,
        const QPoint &globalPosition,
        Qt::KeyboardModifiers modifiers);

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions)

        QList<QUrl> urls;
        for (QListWidgetItem *item : selectedItems()) {
            const QUrl url(item->data(Qt::UserRole).toString());
            if (url.isValid()) {
                urls.push_back(url);
            }
        }

        if (urls.isEmpty()) {
            return;
        }

        auto *mimeData = new QMimeData;
        mimeData->setUrls(urls);

        QDrag drag(this);
        drag.setMimeData(mimeData);
        drag.exec(
            Qt::CopyAction | Qt::MoveAction,
            Qt::CopyAction);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            const QUrl destination =
                dropDestinationAt(event->position().toPoint());
            if (!destination.isValid()) {
                event->ignore();
                return;
            }
            event->setDropAction(
                dropActionForUrls(
                    event->mimeData()->urls(),
                    destination,
                    event->modifiers()));
            event->accept();
            return;
        }
        QListWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            const QUrl destination =
                dropDestinationAt(event->position().toPoint());
            if (!destination.isValid()) {
                event->ignore();
                return;
            }
            event->setDropAction(
                dropActionForUrls(
                    event->mimeData()->urls(),
                    destination,
                    event->modifiers()));
            event->accept();
            return;
        }
        QListWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!event->mimeData()->hasUrls()) {
            QListWidget::dropEvent(event);
            return;
        }

        const QUrl destination =
            dropDestinationAt(event->position().toPoint());
        if (!destination.isValid()) {
            event->ignore();
            return;
        }

        const QPoint globalPosition =
            viewport()->mapToGlobal(event->position().toPoint());

        Q_EMIT urlsDropped(
            event->mimeData()->urls(),
            destination,
            globalPosition,
            event->modifiers());

        event->setDropAction(
            dropActionForUrls(
                event->mimeData()->urls(),
                destination,
                event->modifiers()));
        event->accept();
    }

    void scrollContentsBy(int dx, int dy) override
    {
        QListWidget::scrollContentsBy(dx, dy);
        updateNameOverlay();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QListWidget::resizeEvent(event);
        updateNameOverlay();
    }

    void showEvent(QShowEvent *event) override
    {
        QListWidget::showEvent(event);
        QTimer::singleShot(0, this, [this] {
            updateNameOverlay();
        });
    }

private:
    bool selectedNameNeedsOverlay(QListWidgetItem *item) const
    {
        if (!item || !m_nameOverlay || !viewport()) {
            return false;
        }

        const QString text = item->text();
        if (text.isEmpty()) {
            return false;
        }

        const QFontMetrics metrics(font());
        if (viewMode() == QListView::ListMode) {
            const QRect rect = visualItemRect(item);
            const int available = qMax(40, rect.width() - iconSize().width() - 24);
            if (!alwaysShowFullNames()) {
                return metrics.horizontalAdvance(text) > available;
            }
            const QRect fullRect = metrics.boundingRect(
                QRect(0, 0, available, 10000),
                Qt::TextWordWrap | Qt::AlignLeft,
                text);
            return fullRect.height() > 3 * metrics.lineSpacing();
        }

        constexpr int textWidth = 124;
        if (!alwaysShowFullNames()) {
            return metrics.horizontalAdvance(text) > textWidth;
        }
        const QRect fullRect = metrics.boundingRect(
            QRect(0, 0, textWidth, 10000),
            Qt::TextWordWrap | Qt::AlignHCenter,
            text);
        return fullRect.height() > 3 * metrics.lineSpacing();
    }

    void updateNameOverlay()
    {
        if (!m_nameOverlay || !viewport()) {
            return;
        }

        QListWidgetItem *item = currentItem();
        if (!item || !item->isSelected() || !selectedNameNeedsOverlay(item)) {
            m_nameOverlay->hide();
            return;
        }

        const QRect itemRect = visualItemRect(item);
        if (!itemRect.isValid() || !viewport()->rect().intersects(itemRect)) {
            m_nameOverlay->hide();
            return;
        }

        const QString text = item->text();
        const QFontMetrics metrics(font());
        int overlayWidth = 0;
        int overlayX = 0;
        int overlayY = 0;
        Qt::Alignment alignment = Qt::AlignHCenter | Qt::AlignTop;

        if (viewMode() == QListView::ListMode) {
            overlayWidth = qMin(
                qMax(260, itemRect.width() - iconSize().width()),
                qMax(120, viewport()->width() - 12));
            overlayX = qBound(
                4,
                itemRect.left() + iconSize().width() + 8,
                qMax(4, viewport()->width() - overlayWidth - 4));
            overlayY = itemRect.top() + 2;
            alignment = Qt::AlignLeft | Qt::AlignTop;
        } else {
            overlayWidth = qMax(120, itemRect.width() - 4);
            overlayX = qBound(
                2,
                itemRect.center().x() - overlayWidth / 2,
                qMax(2, viewport()->width() - overlayWidth - 2));
            overlayY = itemRect.top() + iconSize().height() + 10;
        }

        const QRect textRect = metrics.boundingRect(
            QRect(0, 0, qMax(40, overlayWidth - 10), 10000),
            Qt::TextWordWrap | alignment,
            text);
        const int overlayHeight = qMax(
            metrics.lineSpacing() + 10,
            textRect.height() + 10);

        m_nameOverlay->setAlignment(alignment);
        m_nameOverlay->setText(text);
        m_nameOverlay->setGeometry(
            overlayX,
            overlayY,
            overlayWidth,
            overlayHeight);
        m_nameOverlay->raise();
        m_nameOverlay->show();
    }

    QUrl dropDestinationAt(const QPoint &position) const
    {
        QUrl destination = m_dropDirectory;
        QListWidgetItem *target = itemAt(position);
        if (target
            && target->data(Qt::UserRole + 1).toBool()) {
            const QUrl targetUrl(
                target->data(Qt::UserRole).toString());
            if (targetUrl.isValid()) {
                destination = targetUrl;
            }
        }
        return destination;
    }

    QUrl m_dropDirectory;
    ExplorerNameDelegate *m_nameDelegate = nullptr;
    QLabel *m_nameOverlay = nullptr;
};

class DirectoryTreeWidget : public QTreeWidget
{
    Q_OBJECT

public:
    explicit DirectoryTreeWidget(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
    }

    void setDropDirectory(const QUrl &url)
    {
        m_dropDirectory = url;
    }

Q_SIGNALS:
    void urlsDropped(
        const QList<QUrl> &urls,
        const QUrl &destination,
        const QPoint &globalPosition,
        Qt::KeyboardModifiers modifiers);

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions)

        QList<QUrl> urls;
        for (QTreeWidgetItem *item : selectedItems()) {
            const QUrl url(
                item->data(0, Qt::UserRole).toString());
            if (url.isValid()) {
                urls.push_back(url);
            }
        }

        if (urls.isEmpty()) {
            return;
        }

        auto *mimeData = new QMimeData;
        mimeData->setUrls(urls);

        QDrag drag(this);
        drag.setMimeData(mimeData);
        drag.exec(
            Qt::CopyAction | Qt::MoveAction,
            Qt::CopyAction);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            const QUrl destination =
                dropDestinationAt(event->position().toPoint());
            if (!destination.isValid()) {
                event->ignore();
                return;
            }
            event->setDropAction(
                dropActionForUrls(
                    event->mimeData()->urls(),
                    destination,
                    event->modifiers()));
            event->accept();
            return;
        }
        QTreeWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            const QUrl destination =
                dropDestinationAt(event->position().toPoint());
            if (!destination.isValid()) {
                event->ignore();
                return;
            }
            event->setDropAction(
                dropActionForUrls(
                    event->mimeData()->urls(),
                    destination,
                    event->modifiers()));
            event->accept();
            return;
        }
        QTreeWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!event->mimeData()->hasUrls()) {
            QTreeWidget::dropEvent(event);
            return;
        }

        const QUrl destination =
            dropDestinationAt(event->position().toPoint());
        if (!destination.isValid()) {
            event->ignore();
            return;
        }

        const QPoint globalPosition =
            viewport()->mapToGlobal(
                event->position().toPoint());

        Q_EMIT urlsDropped(
            event->mimeData()->urls(),
            destination,
            globalPosition,
            event->modifiers());

        event->setDropAction(
            dropActionForUrls(
                event->mimeData()->urls(),
                destination,
                event->modifiers()));
        event->accept();
    }

private:
    QUrl dropDestinationAt(const QPoint &position) const
    {
        QUrl destination = m_dropDirectory;
        QTreeWidgetItem *target = itemAt(position);
        if (target
            && target->data(
                0,
                Qt::UserRole + 1).toBool()) {
            const QUrl targetUrl(
                target->data(
                    0,
                    Qt::UserRole).toString());
            if (targetUrl.isValid()) {
                destination = targetUrl;
            }
        }
        return destination;
    }

    QUrl m_dropDirectory;
};


// File drags use the normal QWidget event path; tab reordering stays in QTabBar.
class ExplorerTabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit ExplorerTabBar(QWidget *parent = nullptr)
        : QTabBar(parent)
    {
        setAcceptDrops(true);
        m_hoverTimer.setSingleShot(true);
        m_hoverTimer.setInterval(650);
        connect(&m_hoverTimer, &QTimer::timeout, this, [this] {
            const int index = m_hoverTab;
            const bool valid = index >= 0 && tabAt(m_hoverPosition) == index
                && isTabEnabled(index) && isTabVisible(index)
                && m_dropDirectory && m_dropDirectory(index).isValid();
            cancelHover();
            if (valid) setCurrentIndex(index);
        });
        connect(this, &QTabBar::tabMoved, this, [this] { cancelHover(); });
    }

    void setDropDirectoryResolver(std::function<QUrl(int)> resolver)
    {
        m_dropDirectory = std::move(resolver);
    }

Q_SIGNALS:
    void urlsDropped(const QList<QUrl> &urls, const QUrl &destination,
                     const QPoint &globalPosition,
                     Qt::KeyboardModifiers modifiers);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        updateDrag(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        updateDrag(event);
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        cancelHover();
        event->accept();
    }

    void dropEvent(QDropEvent *event) override
    {
        const QUrl destination = dropDestination(event);
        cancelHover();
        if (!destination.isValid()) {
            event->ignore();
            return;
        }
        const QList<QUrl> urls = event->mimeData()->urls();
        const QPoint globalPosition = mapToGlobal(event->position().toPoint());
        event->setDropAction(
            dropActionForUrls(urls, destination, event->modifiers()));
        event->accept();
        Q_EMIT urlsDropped(
            urls,
            destination,
            globalPosition,
            event->modifiers());
    }

    void tabInserted(int index) override
    {
        cancelHover();
        QTabBar::tabInserted(index);
    }

    void tabRemoved(int index) override
    {
        cancelHover();
        QTabBar::tabRemoved(index);
    }

private:
    QUrl dropDestination(const QDropEvent *event) const
    {
        if (!event->mimeData()->hasUrls() || event->mimeData()->urls().isEmpty()
            || event->proposedAction() == Qt::IgnoreAction || !m_dropDirectory) {
            return {};
        }
        const int index = tabAt(event->position().toPoint());
        if (index < 0 || !isTabEnabled(index) || !isTabVisible(index)) {
            return {};
        }
        return m_dropDirectory(index);
    }

    void updateDrag(QDragMoveEvent *event)
    {
        if (!dropDestination(event).isValid()) {
            cancelHover();
            event->ignore();
            return;
        }
        const QPoint position = event->position().toPoint();
        const int index = tabAt(position);
        if (index == currentIndex()) {
            cancelHover();
        } else if (index != m_hoverTab) {
            cancelHover();
            m_hoverTab = index;
            m_hoverTimer.start();
        }
        m_hoverPosition = position;
        event->setDropAction(
            dropActionForUrls(
                event->mimeData()->urls(),
                dropDestination(event),
                event->modifiers()));
        event->accept();
    }

    void cancelHover()
    {
        m_hoverTimer.stop();
        m_hoverTab = -1;
    }

    std::function<QUrl(int)> m_dropDirectory;
    QTimer m_hoverTimer;
    int m_hoverTab = -1;
    QPoint m_hoverPosition;
};


class SplitBrowserPane : public QFrame
{
    Q_OBJECT

public:
    explicit SplitBrowserPane(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("splitBrowserPane"));
        setMinimumWidth(330);
        setProperty("active", false);

        QSettings settings;
        m_viewMode =
            std::clamp(
                settings.value(
                    QStringLiteral("directory/viewMode"),
                    0).toInt(),
                0,
                2);
        m_sortKey =
            std::clamp(
                settings.value(
                    QStringLiteral("directory/sortKey"),
                    0).toInt(),
                0,
                3);
        m_sortAscending =
            settings.value(
                QStringLiteral("directory/sortAscending"),
                true).toBool();
        m_showHiddenFiles =
            settings.value(
                QStringLiteral("directory/showHidden"),
                false).toBool();
        m_thumbnailsEnabled =
            settings.value(
                QStringLiteral("directory/thumbnails"),
                true).toBool();

        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        // --------------------------------------------------------------
        // Navigation row
        // --------------------------------------------------------------
        auto *header = new QFrame(this);
        header->setObjectName(QStringLiteral("splitPaneHeader"));

        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(8, 6, 8, 6);
        headerLayout->setSpacing(4);

        m_backButton = new QToolButton(header);
        m_backButton->setIcon(
            themedIcon(QStringLiteral("go-previous")));
        m_backButton->setToolTip(
            trLocal("Wstecz", "Back"));
        m_backButton->setAutoRaise(true);
        headerLayout->addWidget(m_backButton);

        m_forwardButton = new QToolButton(header);
        m_forwardButton->setIcon(
            themedIcon(QStringLiteral("go-next")));
        m_forwardButton->setToolTip(
            trLocal("Dalej", "Forward"));
        m_forwardButton->setAutoRaise(true);
        headerLayout->addWidget(m_forwardButton);

        m_upButton = new QToolButton(header);
        m_upButton->setIcon(
            themedIcon(QStringLiteral("go-up")));
        m_upButton->setToolTip(
            trLocal("W górę", "Up"));
        m_upButton->setAutoRaise(true);
        headerLayout->addWidget(m_upButton);

        // Pretty breadcrumb by default; click to edit the real address.
        m_locationStack = new QStackedWidget(header);

        m_breadcrumbFrame = new QFrame(m_locationStack);
        m_breadcrumbFrame->setObjectName(
            QStringLiteral("splitBreadcrumbFrame"));

        auto *breadcrumbLayout =
            new QHBoxLayout(m_breadcrumbFrame);
        breadcrumbLayout->setContentsMargins(7, 2, 7, 2);
        breadcrumbLayout->setSpacing(5);

        m_breadcrumbIcon =
            new QLabel(m_breadcrumbFrame);
        m_breadcrumbIcon->setFixedSize(18, 18);
        m_breadcrumbIcon->setAlignment(Qt::AlignCenter);
        breadcrumbLayout->addWidget(m_breadcrumbIcon);

        m_breadcrumbButton =
            new QToolButton(m_breadcrumbFrame);
        m_breadcrumbButton->setObjectName(
            QStringLiteral("splitBreadcrumbButton"));
        m_breadcrumbButton->setAutoRaise(true);
        m_breadcrumbButton->setToolButtonStyle(
            Qt::ToolButtonTextOnly);
        m_breadcrumbButton->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Preferred);
        breadcrumbLayout->addWidget(
            m_breadcrumbButton,
            1);

        m_addressEdit =
            new QLineEdit(m_locationStack);
        m_addressEdit->setObjectName(
            QStringLiteral("splitAddressEdit"));
        m_addressEdit->setClearButtonEnabled(true);
        m_addressEdit->setPlaceholderText(
            trLocal(
                "Wpisz ścieżkę",
                "Enter a path"));

        m_locationStack->addWidget(
            m_breadcrumbFrame);
        m_locationStack->addWidget(
            m_addressEdit);
        m_locationStack->setCurrentWidget(
            m_breadcrumbFrame);

        headerLayout->addWidget(
            m_locationStack,
            1);

        m_viewButton = new QToolButton(header);
        m_viewButton->setAutoRaise(true);
        m_viewButton->setPopupMode(
            QToolButton::InstantPopup);
        m_viewButton->setToolTip(
            trLocal("Widok", "View"));

        auto *viewMenu =
            new QMenu(m_viewButton);
        auto *viewGroup =
            new QActionGroup(viewMenu);
        viewGroup->setExclusive(true);

        struct ViewDef {
            int mode;
            const char *pl;
            const char *en;
            const char *icon;
        };

        const ViewDef viewDefs[] = {
            {0, "Ikony", "Icons", "view-list-icons"},
            {1, "Lista", "List", "view-list-text"},
            {2, "Szczegóły", "Details", "view-list-details"}
        };

        for (const ViewDef &def : viewDefs) {
            QAction *action =
                viewMenu->addAction(
                    themedIcon(
                        QString::fromLatin1(
                            def.icon)),
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setData(def.mode);
            action->setChecked(
                def.mode == m_viewMode);
            viewGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, mode = def.mode] {
                    setViewMode(mode);
                });
        }

        m_viewButton->setMenu(viewMenu);
        headerLayout->addWidget(m_viewButton);

        m_sortButton = new QToolButton(header);
        m_sortButton->setAutoRaise(true);
        m_sortButton->setPopupMode(
            QToolButton::InstantPopup);
        m_sortButton->setToolTip(
            trLocal("Sortuj", "Sort"));

        auto *sortMenu =
            new QMenu(m_sortButton);
        auto *sortGroup =
            new QActionGroup(sortMenu);
        sortGroup->setExclusive(true);

        struct SortDef {
            int key;
            const char *pl;
            const char *en;
        };

        const SortDef sortDefs[] = {
            {0, "Nazwa", "Name"},
            {1, "Typ", "Type"},
            {2, "Rozmiar", "Size"},
            {3, "Data modyfikacji", "Date modified"}
        };

        for (const SortDef &def : sortDefs) {
            QAction *action =
                sortMenu->addAction(
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setChecked(
                def.key == m_sortKey);
            sortGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, key = def.key] {
                    m_sortKey = key;
                    renderItems();
                    updateSortIcon();
                    Q_EMIT stateChanged();
                });
        }

        sortMenu->addSeparator();

        m_sortAscendingAction =
            sortMenu->addAction(
                themedIcon(
                    QStringLiteral(
                        "view-sort-ascending")),
                trLocal(
                    "Rosnąco",
                    "Ascending"));
        m_sortAscendingAction->setCheckable(true);
        m_sortAscendingAction->setChecked(
            m_sortAscending);

        connect(
            m_sortAscendingAction,
            &QAction::toggled,
            this,
            [this](bool checked) {
                m_sortAscending = checked;
                renderItems();
                updateSortIcon();
                Q_EMIT stateChanged();
            });

        m_sortButton->setMenu(sortMenu);
        headerLayout->addWidget(m_sortButton);

        m_swapButton = new QToolButton(header);
        m_swapButton->setIcon(
            themedIcon(
                QStringLiteral(
                    "object-flip-horizontal"),
                QStringLiteral(
                    "transform-move")));
        m_swapButton->setToolTip(
            trLocal(
                "Zamień lokalizacje paneli",
                "Swap pane locations"));
        m_swapButton->setAutoRaise(true);
        headerLayout->addWidget(m_swapButton);

        m_closeButton = new QToolButton(header);
        m_closeButton->setIcon(
            themedIcon(
                QStringLiteral("window-close")));
        m_closeButton->setToolTip(
            trLocal(
                "Wyłącz widok dzielony (F3)",
                "Close split view (F3)"));
        m_closeButton->setAutoRaise(true);
        headerLayout->addWidget(m_closeButton);

        outer->addWidget(header);

        // --------------------------------------------------------------
        // Same heading/count layout as the primary pane
        // --------------------------------------------------------------
        auto *contentHeader =
            new QWidget(this);
        auto *contentHeaderLayout =
            new QVBoxLayout(contentHeader);
        contentHeaderLayout->setContentsMargins(
            16, 12, 16, 7);
        contentHeaderLayout->setSpacing(3);

        m_title = new QLabel(contentHeader);
        QFont titleFont = m_title->font();
        titleFont.setPointSize(
            titleFont.pointSize() + 2);
        titleFont.setBold(true);
        m_title->setFont(titleFont);
        contentHeaderLayout->addWidget(m_title);

        m_status = new QLabel(contentHeader);
        m_status->setObjectName(
            QStringLiteral("splitPaneStatus"));
        m_status->setForegroundRole(
            QPalette::PlaceholderText);
        contentHeaderLayout->addWidget(m_status);

        outer->addWidget(contentHeader);

        // --------------------------------------------------------------
        // Icons/List + Details, matching the main pane
        // --------------------------------------------------------------
        m_viewStack = new QStackedWidget(this);

        m_list =
            new DirectoryListWidget(m_viewStack);
        m_list->setObjectName(
            QStringLiteral("splitDirectoryList"));
        m_list->setResizeMode(QListView::Adjust);
        m_list->setMovement(QListView::Static);
        m_list->setSelectionMode(
            QAbstractItemView::ExtendedSelection);
        m_list->setContextMenuPolicy(
            Qt::CustomContextMenu);

        m_details =
            new DirectoryTreeWidget(m_viewStack);
        m_details->setObjectName(
            QStringLiteral("splitDirectoryDetails"));
        m_details->setColumnCount(4);
        m_details->setHeaderLabels({
            trLocal("Nazwa", "Name"),
            trLocal("Typ", "Type"),
            trLocal("Rozmiar", "Size"),
            trLocal(
                "Zmodyfikowano",
                "Date modified")
        });
        m_details->setRootIsDecorated(false);
        m_details->setUniformRowHeights(true);
        m_details->setAllColumnsShowFocus(true);
        m_details->setSelectionMode(
            QAbstractItemView::ExtendedSelection);
        m_details->setContextMenuPolicy(
            Qt::CustomContextMenu);
        m_details->setIconSize(QSize(22, 22));

        QHeaderView *detailsHeader =
            m_details->header();
        detailsHeader->setStretchLastSection(false);
        detailsHeader->setSectionsMovable(true);
        detailsHeader->setSectionResizeMode(
            0,
            QHeaderView::Stretch);
        detailsHeader->setSectionResizeMode(
            1,
            QHeaderView::Interactive);
        detailsHeader->setSectionResizeMode(
            2,
            QHeaderView::ResizeToContents);
        detailsHeader->setSectionResizeMode(
            3,
            QHeaderView::ResizeToContents);
        m_details->setColumnWidth(1, 190);

        m_viewStack->addWidget(m_list);
        m_viewStack->addWidget(m_details);
        outer->addWidget(m_viewStack, 1);

        connect(
            m_backButton,
            &QToolButton::clicked,
            this,
            &SplitBrowserPane::goBack);
        connect(
            m_forwardButton,
            &QToolButton::clicked,
            this,
            &SplitBrowserPane::goForward);
        connect(
            m_upButton,
            &QToolButton::clicked,
            this,
            &SplitBrowserPane::goUp);
        connect(
            m_closeButton,
            &QToolButton::clicked,
            this,
            &SplitBrowserPane::closeRequested);
        connect(
            m_swapButton,
            &QToolButton::clicked,
            this,
            &SplitBrowserPane::swapRequested);

        connect(
            m_breadcrumbButton,
            &QToolButton::clicked,
            this,
            [this] {
                m_addressEdit->setText(
                    urlForDisplay(
                        m_currentUrl));
                m_locationStack->setCurrentWidget(
                    m_addressEdit);
                m_addressEdit->setFocus(
                    Qt::ShortcutFocusReason);
                m_addressEdit->selectAll();
            });

        connect(
            m_addressEdit,
            &QLineEdit::returnPressed,
            this,
            [this] {
                const QUrl target =
                    urlFromUserText(
                        m_addressEdit->text());
                m_locationStack->setCurrentWidget(
                    m_breadcrumbFrame);
                if (target.isValid()) {
                    navigateTo(target, true);
                }
            });

        auto *escapeAddress =
            new QShortcut(
                QKeySequence(Qt::Key_Escape),
                m_addressEdit);
        connect(
            escapeAddress,
            &QShortcut::activated,
            this,
            [this] {
                m_locationStack->setCurrentWidget(
                    m_breadcrumbFrame);
                updateLocationPresentation();
            });

        connect(
            m_list,
            &QListWidget::itemDoubleClicked,
            this,
            [this](QListWidgetItem *item) {
                activateListItem(item);
            });
        connect(
            m_details,
            &QTreeWidget::itemDoubleClicked,
            this,
            [this](
                QTreeWidgetItem *item,
                int) {
                activateDetailsItem(item);
            });

        connect(
            m_list,
            &QWidget::customContextMenuRequested,
            this,
            &SplitBrowserPane::showListContextMenu);
        connect(
            m_details,
            &QWidget::customContextMenuRequested,
            this,
            &SplitBrowserPane::showDetailsContextMenu);

        connect(
            m_list,
            &DirectoryListWidget::urlsDropped,
            this,
            &SplitBrowserPane::urlsDropped);
        connect(
            m_details,
            &DirectoryTreeWidget::urlsDropped,
            this,
            &SplitBrowserPane::urlsDropped);

        connect(m_list, &QListWidget::itemSelectionChanged,
                this, &SplitBrowserPane::selectionChanged);
        connect(m_details, &QTreeWidget::itemSelectionChanged,
                this, &SplitBrowserPane::selectionChanged);

        applyViewMode();
        updateSortIcon();
        updateNavigationButtons();
    }

    DirectoryListWidget *listView() const { return m_list; }
    DirectoryTreeWidget *detailsView() const { return m_details; }
    QWidget *shortcutScope() const { return m_viewStack; }
    bool canGoBack() const { return m_historyIndex > 0; }
    bool canGoForward() const { return m_historyIndex >= 0 && m_historyIndex + 1 < m_history.size(); }
    bool canGoUp() const { return parentUrl().isValid(); }
    void setDisplayOptions(bool hidden, bool thumbnails)
    {
        m_showHiddenFiles = hidden;
        m_thumbnailsEnabled = thumbnails;
        refresh();
    }

    void setAlwaysShowFullNames(bool enabled)
    {
        if (m_list) {
            m_list->setAlwaysShowFullNames(enabled);
        }
    }
    void navigateBack() { goBack(); }
    void navigateForward() { goForward(); }
    void navigateUp() { goUp(); }

    QUrl currentUrl() const
    {
        return m_currentUrl;
    }

    int viewMode() const
    {
        return m_viewMode;
    }

    int sortKey() const
    {
        return m_sortKey;
    }

    bool sortAscending() const
    {
        return m_sortAscending;
    }

    void setViewMode(int mode)
    {
        m_viewMode =
            std::clamp(mode, 0, 2);
        applyViewMode();
        Q_EMIT stateChanged();
    }

    void setSortState(
        int key,
        bool ascending)
    {
        m_sortKey =
            std::clamp(key, 0, 3);
        m_sortAscending = ascending;

        if (m_sortAscendingAction) {
            QSignalBlocker blocker(
                m_sortAscendingAction);
            m_sortAscendingAction->setChecked(
                ascending);
        }

        if (m_sortButton
            && m_sortButton->menu()) {
            const QList<QAction *> actions =
                m_sortButton->menu()->actions();
            int index = 0;
            for (QAction *action : actions) {
                if (!action->isCheckable()
                    || action
                        == m_sortAscendingAction) {
                    continue;
                }
                QSignalBlocker blocker(action);
                action->setChecked(
                    index == m_sortKey);
                ++index;
                if (index >= 4) {
                    break;
                }
            }
        }

        updateSortIcon();
        renderItems();
    }

    void setCurrentUrl(
        const QUrl &url,
        bool addHistory = true)
    {
        navigateTo(url, addHistory);
    }

    void refresh()
    {
        loadDirectory(m_currentUrl);
    }

    void focusView()
    {
        if (m_viewMode == 2) {
            m_details->setFocus(
                Qt::ShortcutFocusReason);
        } else {
            m_list->setFocus(
                Qt::ShortcutFocusReason);
        }
    }

    bool viewHasFocus() const
    {
        QWidget *focus =
            QApplication::focusWidget();

        return focus
            && (focus == m_list
                || m_list->isAncestorOf(focus)
                || focus == m_details
                || m_details->isAncestorOf(focus));
    }

Q_SIGNALS:
    void selectionChanged();
    void contextMenuRequested(bool details, const QPoint &position);
    void closeRequested();
    void swapRequested();
    void openInPrimaryRequested(
        const QUrl &url);
    void openInNewTabRequested(
        const QUrl &url);
    void openInNewWindowRequested(
        const QUrl &url);
    void stateChanged();
    void urlsDropped(
        const QList<QUrl> &urls,
        const QUrl &destination,
        const QPoint &globalPosition,
        Qt::KeyboardModifiers modifiers);

private:
    QString friendlyLocationText(
        const QUrl &url) const
    {
        if (sameLocation(url, kThisPcUrl)) {
            return trLocal(
                "Ten komputer",
                "This PC");
        }

        if (url.isLocalFile()) {
            const QString path =
                QDir::cleanPath(
                    url.toLocalFile());

            QStorageInfo storage(path);
            QString root =
                QDir::cleanPath(
                    storage.rootPath());
            QString rootName =
                storage.displayName();

            if (rootName.trimmed().isEmpty()) {
                rootName =
                    QFileInfo(root).fileName();
            }

            if (rootName.trimmed().isEmpty()
                || root == QStringLiteral("/")) {
                rootName =
                    trLocal(
                        "System",
                        "System");
            }

            QString result =
                trLocal(
                    "Ten komputer",
                    "This PC")
                + QStringLiteral("  ›  ")
                + rootName;

            QString relative =
                QDir(root).relativeFilePath(path);

            if (!relative.isEmpty()
                && relative != QStringLiteral(".")) {
                const QStringList parts =
                    relative.split(
                        QLatin1Char('/'),
                        Qt::SkipEmptyParts);

                for (const QString &part : parts) {
                    result +=
                        QStringLiteral("  ›  ")
                        + part;
                }
            }

            return result;
        }

        if (isAdminUrl(url)) {
            QString result =
                trLocal(
                    "Administrator",
                    "Administrator");

            const QStringList parts =
                url.path().split(
                    QLatin1Char('/'),
                    Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                result +=
                    QStringLiteral("  ›  ")
                    + part;
            }
            return result;
        }

        return urlForDisplay(url);
    }

    QString friendlyTitle(
        const QUrl &url) const
    {
        if (sameLocation(url, kThisPcUrl)) {
            return trLocal(
                "Ten komputer",
                "This PC");
        }

        if (url.isLocalFile()) {
            const QString path =
                QDir::cleanPath(
                    url.toLocalFile());
            const QFileInfo info(path);

            QStorageInfo storage(path);
            const QString root =
                QDir::cleanPath(
                    storage.rootPath());

            if (path == root) {
                QString label =
                    storage.displayName();
                if (!label.trimmed().isEmpty()) {
                    return label;
                }
            }

            if (!info.fileName().isEmpty()) {
                return info.fileName();
            }
        }

        const QString path =
            url.path();
        const QString name =
            QFileInfo(path).fileName();

        if (!name.isEmpty()) {
            return name;
        }

        return urlForDisplay(url);
    }

    QIcon locationIcon(
        const QUrl &url) const
    {
        if (sameLocation(url, kThisPcUrl)) {
            return themedIcon(
                QStringLiteral("computer"));
        }

        if (isAdminUrl(url)) {
            return themedIcon(
                QStringLiteral("security-high"));
        }

        if (url.isLocalFile()) {
            const QString path =
                QDir::cleanPath(
                    url.toLocalFile());
            QStorageInfo storage(path);

            if (QDir::cleanPath(
                    storage.rootPath())
                == path) {
                return themedIcon(
                    QStringLiteral(
                        "drive-harddisk"));
            }
        }

        return themedIcon(
            QStringLiteral("folder"));
    }

    void updateLocationPresentation()
    {
        m_breadcrumbButton->setText(
            friendlyLocationText(
                m_currentUrl));
        m_breadcrumbButton->setToolTip(
            urlForDisplay(
                m_currentUrl));

        m_breadcrumbIcon->setPixmap(
            locationIcon(
                m_currentUrl).pixmap(
                    16, 16));

        m_title->setText(
            friendlyTitle(
                m_currentUrl));
    }

    QUrl parentUrl() const
    {
        if (!m_currentUrl.isValid()
            || sameLocation(
                m_currentUrl,
                kThisPcUrl)) {
            return {};
        }

        QUrl parent = m_currentUrl;
        QString path = parent.path();

        if (path.isEmpty()
            || path == QStringLiteral("/")) {
            return kThisPcUrl;
        }

        while (path.size() > 1
               && path.endsWith(
                   QLatin1Char('/'))) {
            path.chop(1);
        }

        const int slash =
            path.lastIndexOf(
                QLatin1Char('/'));

        path =
            slash <= 0
                ? QStringLiteral("/")
                : path.left(slash);

        parent.setPath(path);
        parent.setQuery(QString());

        if (parent.isLocalFile()
            && QFileInfo(
                parent.toLocalFile())
                   .absoluteFilePath()
                == QFileInfo(
                    m_currentUrl.toLocalFile())
                       .absoluteFilePath()) {
            return kThisPcUrl;
        }

        return normalizedUrl(parent);
    }

    void navigateTo(
        const QUrl &rawUrl,
        bool addHistory)
    {
        if (!rawUrl.isValid()) {
            return;
        }

        const QUrl url =
            normalizedUrl(rawUrl);

        if (addHistory) {
            if (m_historyIndex >= 0
                && m_historyIndex
                    < m_history.size()
                && sameLocation(
                    m_history.at(
                        m_historyIndex),
                    url)) {
                loadDirectory(url);
                return;
            }

            while (m_history.size()
                   > m_historyIndex + 1) {
                m_history.removeLast();
            }

            m_history.push_back(url);
            m_historyIndex =
                m_history.size() - 1;
        }

        m_currentUrl = url;
        loadDirectory(url);
        updateNavigationButtons();
        Q_EMIT stateChanged();
    }

    void goBack()
    {
        if (m_historyIndex <= 0) {
            return;
        }

        --m_historyIndex;
        m_currentUrl =
            m_history.at(m_historyIndex);
        loadDirectory(m_currentUrl);
        updateNavigationButtons();
        Q_EMIT stateChanged();
    }

    void goForward()
    {
        if (m_historyIndex < 0
            || m_historyIndex + 1
                >= m_history.size()) {
            return;
        }

        ++m_historyIndex;
        m_currentUrl =
            m_history.at(m_historyIndex);
        loadDirectory(m_currentUrl);
        updateNavigationButtons();
        Q_EMIT stateChanged();
    }

    void goUp()
    {
        const QUrl parent =
            parentUrl();

        if (parent.isValid()) {
            navigateTo(parent, true);
        }
    }

    void updateNavigationButtons()
    {
        m_backButton->setEnabled(
            m_historyIndex > 0);

        m_forwardButton->setEnabled(
            m_historyIndex >= 0
            && m_historyIndex + 1
                < m_history.size());

        m_upButton->setEnabled(
            parentUrl().isValid());

        updateLocationPresentation();
    }

    void loadDirectory(
        const QUrl &url)
    {
        if (m_job) {
            m_job->kill();
            m_job = nullptr;
        }

        m_pending.clear();
        m_list->clear();
        m_details->clear();

        m_list->setDropDirectory(url);
        m_details->setDropDirectory(url);

        m_currentUrl = url;
        updateLocationPresentation();

        m_status->setText(
            trLocal(
                "Wczytywanie…",
                "Loading…"));

        KIO::ListJob *job =
            KIO::listDir(
                url,
                KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        m_job = job;

        connect(
            job,
            &KIO::ListJob::entries,
            this,
            [this, url, job](
                KIO::Job *,
                const KIO::UDSEntryList &entries) {
            if (m_job != job) {
                return;
            }

            for (const KIO::UDSEntry &entry :
                 entries) {
                const QString rawName =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_NAME);

                if (rawName.isEmpty()
                    || rawName
                        == QStringLiteral(".")
                    || rawName
                        == QStringLiteral("..")
                    || (!m_showHiddenFiles
                        && rawName.startsWith(
                            QLatin1Char('.')))) {
                    continue;
                }

                FileInfo file;
                file.name =
                    entry.stringValue(
                        KIO::UDSEntry::
                            UDS_DISPLAY_NAME);

                if (file.name.isEmpty()) {
                    file.name = rawName;
                }

                file.mimeType =
                    entry.stringValue(
                        KIO::UDSEntry::
                            UDS_MIME_TYPE);
                file.iconName =
                    entry.stringValue(
                        KIO::UDSEntry::
                            UDS_ICON_NAME);
                file.url =
                    childUrlForEntry(
                        url,
                        entry);
                file.isDir =
                    entryIsDirectory(entry);
                file.size =
                    entry.numberValue(
                        KIO::UDSEntry::
                            UDS_SIZE,
                        -1);
                file.modificationTime =
                    entry.numberValue(
                        KIO::UDSEntry::
                            UDS_MODIFICATION_TIME,
                        0);

                m_pending.push_back(file);
            }
        });

        connect(
            job,
            &KJob::result,
            this,
            [this, job](KJob *) {
            if (m_job != job) {
                return;
            }

            m_job = nullptr;

            if (job->error()) {
                m_status->setText(
                    job->errorString());
                return;
            }

            renderItems();
        });
    }

    QIcon iconForSplitFile(
        const FileInfo &file,
        QMimeDatabase &database)
    {
        const QMimeType mime =
            resolvedMimeType(
                file,
                database);

        QString iconName =
            file.iconName;

        if (iconName.isEmpty()
            || iconName
                == QStringLiteral(
                    "text-x-generic")) {
            if (mime.isValid()
                && !mime.isDefault()) {
                iconName =
                    mime.iconName();

                if (iconName.isEmpty()) {
                    iconName =
                        mime.genericIconName();
                }
            }
        }

        if (iconName.isEmpty()) {
            iconName =
                file.isDir
                    ? QStringLiteral("folder")
                    : QStringLiteral(
                        "text-x-generic");
        }

        const QIcon fallback =
            themedIcon(
                iconName,
                file.isDir
                    ? QStringLiteral("folder")
                    : QStringLiteral(
                        "text-x-generic"));

        const QString mimeName =
            mime.isValid()
                ? mime.name()
                : QString();

        if (!m_thumbnailsEnabled
            || file.isDir
            || !file.url.isLocalFile()
            || !mimeName.startsWith(
                QStringLiteral("image/"))
            || file.size
                > 64LL * 1024LL * 1024LL) {
            return fallback;
        }

        const QString path =
            file.url.toLocalFile();

        QImageReader reader(path);
        reader.setAutoTransform(true);

        const QSize source =
            reader.size();
        const QSize target(128, 128);

        if (source.isValid()
            && (source.width()
                    > target.width()
                || source.height()
                    > target.height())) {
            reader.setScaledSize(
                source.scaled(
                    target,
                    Qt::KeepAspectRatio));
        }

        const QImage image =
            reader.read();

        if (image.isNull()) {
            return fallback;
        }

        QPixmap pixmap =
            QPixmap::fromImage(image)
                .scaled(
                    target,
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation);

        return QIcon(pixmap);
    }

    void renderItems()
    {
        QMimeDatabase database;

        auto typeFor =
            [&database](
                const FileInfo &file) {
            return fileTypeLabel(
                file,
                database);
        };

        std::sort(
            m_pending.begin(),
            m_pending.end(),
            [this, &typeFor](
                const FileInfo &a,
                const FileInfo &b) {
            if (a.isDir != b.isDir) {
                return a.isDir;
            }

            int comparison = 0;

            switch (m_sortKey) {
            case 1:
                comparison =
                    typeFor(a)
                        .localeAwareCompare(
                            typeFor(b));
                break;

            case 2:
                if (a.size < b.size) {
                    comparison = -1;
                } else if (a.size > b.size) {
                    comparison = 1;
                }
                break;

            case 3:
                if (a.modificationTime
                    < b.modificationTime) {
                    comparison = -1;
                } else if (
                    a.modificationTime
                    > b.modificationTime) {
                    comparison = 1;
                }
                break;

            case 0:
            default:
                comparison =
                    a.name.localeAwareCompare(
                        b.name);
                break;
            }

            if (comparison == 0) {
                comparison =
                    a.name.localeAwareCompare(
                        b.name);
            }

            return m_sortAscending
                ? comparison < 0
                : comparison > 0;
        });

        m_list->clear();
        m_details->clear();

        for (const FileInfo &file :
             std::as_const(m_pending)) {
            const QIcon icon =
                iconForSplitFile(
                    file,
                    database);

            const QString typeText =
                fileTypeLabel(
                    file,
                    database);

            const QString sizeText =
                formatFileSize(
                    file.size,
                    file.isDir);

            const QString modifiedText =
                formatModificationTime(
                    file.modificationTime);

            auto *listItem =
                new QListWidgetItem(
                    icon,
                    file.name,
                    m_list);

            listItem->setData(
                Qt::UserRole,
                file.url.toString());
            listItem->setData(
                Qt::UserRole + 1,
                file.isDir);
            listItem->setData(
                Qt::UserRole + 2,
                typeText);
            listItem->setData(
                Qt::UserRole + 3,
                sizeText);
            listItem->setData(
                Qt::UserRole + 4,
                modifiedText);
            listItem->setToolTip(
                QStringLiteral(
                    "%1\n%2\n%3")
                    .arg(
                        urlForDisplay(
                            file.url),
                        typeText,
                        modifiedText));

            auto *detailsItem =
                new QTreeWidgetItem(
                    m_details,
                    {
                        file.name,
                        typeText,
                        sizeText,
                        modifiedText
                    });

            detailsItem->setIcon(
                0,
                icon);
            detailsItem->setData(
                0,
                Qt::UserRole,
                file.url.toString());
            detailsItem->setData(
                0,
                Qt::UserRole + 1,
                file.isDir);
            detailsItem->setToolTip(
                0,
                urlForDisplay(
                    file.url));
            detailsItem->setTextAlignment(
                2,
                Qt::AlignRight
                    | Qt::AlignVCenter);
        }

        m_status->setText(
            isPolish()
                ? QStringLiteral(
                    "%1 elementów")
                    .arg(
                        m_pending.size())
                : QStringLiteral(
                    "%1 items")
                    .arg(
                        m_pending.size()));
    }

    void applyViewMode()
    {
        if (m_viewMode == 0) {
            m_list->setViewMode(
                QListView::IconMode);
            m_list->setFlow(
                QListView::LeftToRight);
            m_list->setWrapping(true);
            m_list->setIconSize(
                QSize(64, 64));
            m_list->setGridSize(QSize());
            m_list->setSpacing(3);
            m_list->setUniformItemSizes(
                false);
            m_viewStack->setCurrentWidget(
                m_list);
            m_viewButton->setIcon(
                themedIcon(
                    QStringLiteral(
                        "view-list-icons")));
        } else if (m_viewMode == 1) {
            m_list->setViewMode(
                QListView::ListMode);
            m_list->setFlow(
                QListView::TopToBottom);
            m_list->setWrapping(false);
            m_list->setIconSize(
                QSize(24, 24));
            m_list->setGridSize(QSize());
            m_list->setSpacing(1);
            m_list->setUniformItemSizes(
                false);
            m_viewStack->setCurrentWidget(
                m_list);
            m_viewButton->setIcon(
                themedIcon(
                    QStringLiteral(
                        "view-list-text")));
        } else {
            m_viewStack->setCurrentWidget(
                m_details);
            m_viewButton->setIcon(
                themedIcon(
                    QStringLiteral(
                        "view-list-details")));
        }

        configureDirectoryDragDrop(m_list);
        configureDirectoryDragDrop(m_details);

        if (m_viewButton
            && m_viewButton->menu()) {
            for (QAction *action :
                 m_viewButton->menu()
                     ->actions()) {
                if (!action->isCheckable()) {
                    continue;
                }

                QSignalBlocker blocker(action);
                action->setChecked(
                    action->data().toInt()
                    == m_viewMode);
            }
        }
    }

    void updateSortIcon()
    {
        if (!m_sortButton) {
            return;
        }

        m_sortButton->setIcon(
            themedIcon(
                m_sortAscending
                    ? QStringLiteral(
                        "view-sort-ascending")
                    : QStringLiteral(
                        "view-sort-descending")));
    }

    void activateListItem(
        QListWidgetItem *item)
    {
        if (!item) {
            return;
        }

        const QUrl url(
            item->data(
                Qt::UserRole).toString());

        const bool isDir =
            item->data(
                Qt::UserRole + 1).toBool();

        if (isDir) {
            navigateTo(url, true);
        } else if (url.isValid()) {
            QDesktopServices::openUrl(url);
        }
    }

    void activateDetailsItem(
        QTreeWidgetItem *item)
    {
        if (!item) {
            return;
        }

        const QUrl url(
            item->data(
                0,
                Qt::UserRole).toString());

        const bool isDir =
            item->data(
                0,
                Qt::UserRole + 1).toBool();

        if (isDir) {
            navigateTo(url, true);
        } else if (url.isValid()) {
            QDesktopServices::openUrl(url);
        }
    }

    void showListContextMenu(const QPoint &pos)
    {
        Q_EMIT contextMenuRequested(false, pos);
    }

    void showDetailsContextMenu(const QPoint &pos)
    {
        Q_EMIT contextMenuRequested(true, pos);
    }

    QToolButton *m_backButton = nullptr;
    QToolButton *m_forwardButton = nullptr;
    QToolButton *m_upButton = nullptr;
    QToolButton *m_viewButton = nullptr;
    QToolButton *m_sortButton = nullptr;
    QToolButton *m_swapButton = nullptr;
    QToolButton *m_closeButton = nullptr;
    QAction *m_sortAscendingAction = nullptr;

    QStackedWidget *m_locationStack = nullptr;
    QFrame *m_breadcrumbFrame = nullptr;
    QLabel *m_breadcrumbIcon = nullptr;
    QToolButton *m_breadcrumbButton = nullptr;
    QLineEdit *m_addressEdit = nullptr;

    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;

    QStackedWidget *m_viewStack = nullptr;
    DirectoryListWidget *m_list = nullptr;
    DirectoryTreeWidget *m_details = nullptr;

    QUrl m_currentUrl = kThisPcUrl;
    QList<QUrl> m_history;
    int m_historyIndex = -1;

    int m_viewMode = 0;
    int m_sortKey = 0;
    bool m_sortAscending = true;
    bool m_showHiddenFiles = false;
    bool m_thumbnailsEnabled = true;

    QList<FileInfo> m_pending;
    QPointer<KIO::ListJob> m_job;
};


QString applicationStyleSheet()
{
    return QStringLiteral(R"QSS(
QFrame#thispcCard {
    border: 1px solid transparent;
    border-radius: 8px;
    background: transparent;
}
QFrame#thispcCard:hover {
    background: palette(alternate-base);
    border: 1px solid palette(mid);
}
QFrame#thispcCard:focus {
    background: palette(alternate-base);
    border: 1px solid palette(highlight);
}

QProgressBar#driveProgress,
QProgressBar#sidebarProgress {
    border: none;
    background: palette(mid);
    padding: 0px;
}
QProgressBar#driveProgress {
    border-radius: 4px;
}
QProgressBar#driveProgress::chunk {
    border-radius: 4px;
    background: palette(highlight);
}
QProgressBar#sidebarProgress {
    border-radius: 2px;
    min-height: 5px;
    max-height: 5px;
}
QProgressBar#sidebarProgress::chunk {
    border-radius: 2px;
    background: palette(highlight);
}

QFrame#sidebar {
    border-right: 1px solid palette(mid);
    background: palette(base);
}

QToolButton#sidebarSectionButton {
    border: none;
    background: transparent;
    text-align: left;
    font-weight: 600;
    color: palette(text);
    padding: 10px 6px 5px 6px;
}
QToolButton#sidebarSectionButton:hover {
    background: palette(alternate-base);
    border-radius: 5px;
}

QPushButton#sidebarButton {
    text-align: left;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 5px 9px;
    background: transparent;
}
QPushButton#sidebarButton:hover {
    background: palette(alternate-base);
}
QPushButton#sidebarButton[current="true"] {
    background: palette(alternate-base);
    border: 1px solid palette(highlight);
}

QFrame#sidebarDrive {
    border: 1px solid transparent;
    border-radius: 6px;
    background: transparent;
}
QFrame#sidebarDrive:hover {
    background: palette(alternate-base);
}
QFrame#sidebarDrive:focus,
QFrame#sidebarDrive[current="true"] {
    border: 1px solid palette(highlight);
    background: palette(alternate-base);
}

QFrame#breadcrumbFrame {
    border: 1px solid palette(mid);
    border-radius: 6px;
    background: palette(base);
}
QToolButton#searchFilterButton {
    border: 1px solid palette(mid);
    border-radius: 6px;
    padding: 3px 6px;
    background: palette(base);
}
QToolButton#searchFilterButton:hover {
    background: palette(alternate-base);
}
QFrame#searchProgressFrame {
    background: transparent;
}

QFrame#adminBanner {
    border: 1px solid palette(highlight);
    border-radius: 7px;
    background: palette(alternate-base);
    padding: 3px;
}
QLabel#adminBannerText {
    font-weight: 600;
}
QToolButton#crumbButton {
    border: none;
    border-radius: 4px;
    padding: 3px 6px;
    min-height: 22px;
    background: transparent;
}
QToolButton#crumbButton:hover {
    background: palette(alternate-base);
}

QToolBar#fileCommandToolbar {
    border-top: 1px solid palette(mid);
    border-bottom: 1px solid palette(mid);
    spacing: 4px;
    padding: 3px 5px;
}

QLineEdit#searchEdit {
    min-width: 240px;
    max-width: 260px;
    padding: 4px 7px;
}

QListWidget#directoryList {
    border: none;
    background: transparent;
    outline: none;
}
QListWidget#directoryList::item {
    border: 1px solid transparent;
    border-radius: 7px;
    padding: 6px;
}
QListWidget#directoryList::item:hover {
    background: palette(alternate-base);
}
QListWidget#directoryList::item:selected {
    background: palette(alternate-base);
    border: 1px solid palette(highlight);
}

QFrame#tabStrip {
    border-bottom: 1px solid palette(mid);
    background: palette(base);
}
QSplitter#contentSplitter::handle {
    background: palette(mid);
    width: 1px;
}
QWidget#primaryBrowserPane,
QFrame#splitBrowserPane {
    border: 1px solid transparent;
    background: palette(base);
}
QWidget#primaryBrowserPane[active="true"],
QFrame#splitBrowserPane[active="true"] {
    border-top: 2px solid palette(highlight);
}
QFrame#splitPaneHeader {
    background: palette(window);
    border-bottom: 1px solid palette(mid);
}
QFrame#splitBreadcrumbFrame {
    border: 1px solid palette(mid);
    border-radius: 6px;
    background: palette(base);
}
QToolButton#splitBreadcrumbButton {
    border: none;
    background: transparent;
    text-align: left;
    padding: 3px 2px;
}
QToolButton#splitBreadcrumbButton:hover {
    background: palette(alternate-base);
    border-radius: 4px;
}
QLineEdit#splitAddressEdit {
    min-height: 25px;
}
QLabel#splitPaneStatus {
    color: palette(placeholder-text);
}
QListWidget#splitDirectoryList {
    border: none;
    background: transparent;
    outline: none;
}
QListWidget#splitDirectoryList::item {
    border: 1px solid transparent;
    border-radius: 7px;
    padding: 6px;
}
QListWidget#splitDirectoryList::item:hover {
    background: palette(alternate-base);
}
QListWidget#splitDirectoryList::item:selected {
    background: palette(alternate-base);
    border: 1px solid palette(highlight);
}
QTreeWidget#splitDirectoryDetails {
    border: none;
    background: transparent;
    outline: none;
}
QTreeWidget#splitDirectoryDetails::item:hover {
    background: palette(alternate-base);
}
QTabBar#explorerTabs {
    background: transparent;
}
QTabBar#explorerTabs::tab {
    min-width: 125px;
    max-width: 240px;
    min-height: 30px;
    padding: 4px 10px;
    margin: 3px 1px 0 1px;
    border: 1px solid transparent;
    border-bottom: none;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
QTabBar#explorerTabs::tab:hover {
    background: palette(alternate-base);
}
QTabBar#explorerTabs::tab:selected {
    background: palette(window);
    border-color: palette(mid);
}
QToolButton#newTabButton {
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 4px 7px;
    margin: 3px 4px 1px 3px;
}
QToolButton#newTabButton:hover {
    background: palette(alternate-base);
    border-color: palette(mid);
}

QTreeWidget#directoryDetails {
    border: none;
    background: transparent;
    outline: none;
}
QTreeWidget#directoryDetails::item {
    min-height: 28px;
    border: 1px solid transparent;
}
QTreeWidget#directoryDetails::item:hover {
    background: palette(alternate-base);
}
QTreeWidget#directoryDetails::item:selected {
    background: palette(alternate-base);
    border: 1px solid palette(highlight);
}
QTreeWidget#directoryDetails QHeaderView::section {
    padding: 5px 7px;
}

QToolButton#operationButton {
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 5px;
    margin: 1px 5px 1px 3px;
    background: transparent;
}
QToolButton#operationButton:hover,
QToolButton#operationButton[active="true"] {
    background: palette(alternate-base);
    border-color: palette(mid);
}
QToolButton#operationButton::menu-indicator {
    image: none;
    width: 0px;
}
QLabel#operationBadge {
    background: palette(highlight);
    color: palette(highlighted-text);
    border-radius: 8px;
    font-size: 9px;
    font-weight: 700;
}
QLabel#versionLabel {
    color: palette(window-text);
    padding: 0px 7px;
    font-size: 10px;
    font-weight: 600;
}
QFrame#operationPopup {
    border: 1px solid palette(mid);
    border-radius: 9px;
    background: palette(window);
}
QFrame#operationPopupHeader {
    background: transparent;
    border-bottom: 1px solid palette(mid);
}
QLabel#operationPopupTitle {
    font-weight: 700;
    font-size: 14px;
}
QLabel#operationSummaryLabel,
QLabel#operationDetailsLabel,
QLabel#operationEmptyLabel {
    color: palette(placeholder-text);
}
QFrame#operationRow {
    border: 1px solid palette(mid);
    border-radius: 7px;
    background: palette(base);
}
QFrame#operationRow:hover {
    background: palette(alternate-base);
}
QLabel#operationTitleLabel {
    font-weight: 600;
}
QProgressBar#operationProgress {
    border: none;
    border-radius: 3px;
    background: palette(mid);
    min-height: 6px;
    max-height: 6px;
    text-align: center;
}
QProgressBar#operationProgress::chunk {
    border-radius: 3px;
    background: palette(highlight);
}
QProgressBar#operationOverallProgress {
    border: none;
    border-radius: 2px;
    background: palette(mid);
    min-height: 4px;
    max-height: 4px;
}
QProgressBar#operationOverallProgress::chunk {
    border-radius: 2px;
    background: palette(highlight);
}
QFrame#operationPopupFooter {
    border-top: 1px solid palette(mid);
    background: transparent;
}
QToolButton#operationFooterButton {
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 5px 7px;
}
QToolButton#operationFooterButton:hover {
    background: palette(alternate-base);
    border-color: palette(mid);
}
)QSS");
}

} // namespace

class ThisPcWindow : public QMainWindow
{
    Q_OBJECT

    enum class PaneId { Primary, Split };
    struct PaneItem {
        QUrl url;
        QString name, type, size, modified;
        bool isDir = false;
    };
    struct PaneContext {
        PaneId id = PaneId::Primary;
        QUrl directory;
        QAbstractItemView *view = nullptr;
        QList<PaneItem> items;
        bool isDirectory = false;
    };
    PaneId m_activePane = PaneId::Primary;
    const PaneContext *m_operationContext = nullptr;

    PaneContext paneContext() const
    {
        if (m_operationContext) {
            return *m_operationContext;
        }
        PaneContext context;
        const bool split = m_activePane == PaneId::Split
            && m_splitPane && m_splitPane->isVisible();
        context.id = split ? PaneId::Split : PaneId::Primary;
        context.directory = split ? m_splitPane->currentUrl() : m_currentUrl;
        context.isDirectory = split || (m_contentStack
            && m_contentStack->currentWidget() == m_directoryPage);
        const int mode = split ? m_splitPane->viewMode() : m_directoryViewMode;
        auto *list = split ? m_splitPane->listView() : m_directoryList;
        auto *tree = split ? m_splitPane->detailsView() : m_directoryDetails;
        context.view = mode == 2 ? static_cast<QAbstractItemView *>(tree) : list;
        if (!context.isDirectory || !context.view) {
            return context;
        }
        if (mode == 2) {
            for (auto *item : tree->selectedItems()) {
                context.items.push_back({QUrl(item->data(0, Qt::UserRole).toString()),
                    item->text(0), item->text(1), item->text(2), item->text(3),
                    item->data(0, Qt::UserRole + 1).toBool()});
            }
        } else {
            for (auto *item : list->selectedItems()) {
                context.items.push_back({QUrl(item->data(Qt::UserRole).toString()),
                    item->text(), item->data(Qt::UserRole + 2).toString(),
                    item->data(Qt::UserRole + 3).toString(),
                    item->data(Qt::UserRole + 4).toString(),
                    item->data(Qt::UserRole + 1).toBool()});
            }
        }
        return context;
    }

    void setActivePane(PaneId pane)
    {
        m_activePane = pane == PaneId::Split && m_splitPane && m_splitPane->isVisible()
            ? PaneId::Split : PaneId::Primary;
        for (QWidget *widget : {m_primaryPane, static_cast<QWidget *>(m_splitPane)}) {
            if (!widget) continue;
            const bool active = widget == (m_activePane == PaneId::Split
                ? static_cast<QWidget *>(m_splitPane) : m_primaryPane);
            if (widget->property("active").toBool() != active) {
                widget->setProperty("active", active);
                widget->style()->unpolish(widget);
                widget->style()->polish(widget);
                widget->update();
            }
        }
        updateFileActionStates();
    }

    void navigatePane(PaneId pane, const QUrl &url)
    {
        if (pane == PaneId::Split) m_splitPane->setCurrentUrl(url, true);
        else navigateTo(url, true);
    }

    void refreshPane(PaneId pane)
    {
        if (pane == PaneId::Split) m_splitPane->refresh();
        else refreshCurrent();
    }

    void openInOtherPane(PaneId pane, const QUrl &url)
    {
        if (pane == PaneId::Split) navigateTo(url, true);
        else openInSplitPane(url);
    }

    void showSelectedProperties()
    {
        const auto context = paneContext();
        if (context.items.size() != 1) return;
        const auto item = context.items.first();
        showPropertiesDialog(item.name, item.url, item.isDir,
                             item.type, item.size, item.modified);
    }


public:
    explicit ThisPcWindow(
        const QUrl &initialUrl = kThisPcUrl,
        bool allowSessionRestore = true)
    {
        setWindowTitle(trLocal("Ten komputer", "This PC"));
        setWindowIcon(QIcon::fromTheme(QStringLiteral("computer")));

        QSettings settings;
        const QByteArray geometry =
            settings.value(QStringLiteral("window/geometry")).toByteArray();

        if (!geometry.isEmpty()) {
            restoreGeometry(geometry);
        } else {
            resize(1180, 760);
        }

        m_fileUndoManager = KIO::FileUndoManager::self();
        if (m_fileUndoManager && m_fileUndoManager->uiInterface()) {
            m_fileUndoManager->uiInterface()->setParentWidget(this);
            // thispc-view has its own status/operation UI; avoid a second
            // generic KIO progress dialog during undo/redo.
            m_fileUndoManager->uiInterface()->setShowProgressInfo(false);
        }

        // Explorer-like layout: command bar on the first row, navigation /
        // address / search directly below it.
        buildFileActions();
        setupUndoRedoManager();
        buildToolbar();
        buildCentralUi();
        buildSidebar();

        connect(
            QApplication::clipboard(),
            &QClipboard::dataChanged,
            this,
            &ThisPcWindow::updateFileActionStates);

        m_refreshTimer.setInterval(15000);
        connect(
            &m_refreshTimer,
            &QTimer::timeout,
            this,
            &ThisPcWindow::reloadDrives);
        m_refreshTimer.start();

        reloadDrives();

        const bool restored =
            allowSessionRestore
            && m_restorePreviousSession
            && restoreSessionState();
        if (!restored) {
            createNewTab(
                initialUrl.isValid() ? initialUrl : kThisPcUrl,
                true);
        }
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        const int activeOperations = activeFileOperationCount();
        if (activeOperations > 0) {
            const QString question =
                isPolish()
                    ? QStringLiteral(
                        "Trwa %1 operacji plikowych. Zamknięcie aplikacji przerwie aktywne zadania.\n\nCzy na pewno zamknąć?")
                        .arg(activeOperations)
                    : QStringLiteral(
                        "%1 file operations are still running. Closing the application will stop active tasks.\n\nClose anyway?")
                        .arg(activeOperations);

            if (QMessageBox::question(
                    this,
                    trLocal("Aktywne operacje", "Active operations"),
                    question,
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No)
                != QMessageBox::Yes) {
                event->ignore();
                return;
            }

            cancelAllFileOperations();
        }

        saveSessionState();

        QSettings settings;
        settings.setValue(
            QStringLiteral("window/geometry"),
            saveGeometry());
        if (m_contentSplitter) {
            settings.setValue(
                QStringLiteral("split/state"),
                m_contentSplitter->saveState());
        }

        QMainWindow::closeEvent(event);
    }

private:
    struct TabState {
        QUrl currentUrl = kThisPcUrl;
        QList<QUrl> history;
        int historyIndex = -1;
        bool splitEnabled = false;
        QUrl splitUrl = kThisPcUrl;
        int splitViewMode = -1;
        int splitSortKey = 0;
        bool splitSortAscending = true;
    };

    struct FileOperationItem {
        QPointer<KJob> job;
        QFrame *row = nullptr;
        QLabel *titleLabel = nullptr;
        QLabel *detailsLabel = nullptr;
        QLabel *stateLabel = nullptr;
        QProgressBar *progressBar = nullptr;
        QToolButton *cancelButton = nullptr;
        QString contextText;
        qulonglong processedBytes = 0;
        qulonglong totalBytes = 0;
        unsigned long speedBytesPerSecond = 0;
        unsigned long percent = 0;
        bool finished = false;
        bool cancelRequested = false;
        bool failed = false;
    };

    void buildToolbar()
    {
        // The command bar is built first. Force navigation/address/search onto
        // the row below, matching Windows 11 Explorer.
        addToolBarBreak(Qt::TopToolBarArea);

        auto *toolbar = addToolBar(trLocal("Nawigacja", "Navigation"));
        toolbar->setObjectName(QStringLiteral("navigationToolbar"));
        toolbar->setMovable(false);
        toolbar->setFloatable(false);
        toolbar->setIconSize(QSize(20, 20));
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

        m_backAction = toolbar->addAction(
            themedIcon(QStringLiteral("go-previous")),
            trLocal("Wstecz", "Back"));
        m_backAction->setShortcut(QKeySequence::Back);
        connect(m_backAction, &QAction::triggered, this, [this] { if (paneContext().id == PaneId::Split) m_splitPane->navigateBack(); else goBack(); });

        m_forwardAction = toolbar->addAction(
            themedIcon(QStringLiteral("go-next")),
            trLocal("Dalej", "Forward"));
        m_forwardAction->setShortcut(QKeySequence::Forward);
        connect(
            m_forwardAction,
            &QAction::triggered,
            this,
            [this] { if (paneContext().id == PaneId::Split) m_splitPane->navigateForward(); else goForward(); });

        m_upAction = toolbar->addAction(
            themedIcon(QStringLiteral("go-up")),
            trLocal("W górę", "Up"));
        m_upAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
        connect(m_upAction, &QAction::triggered, this, [this] { if (paneContext().id == PaneId::Split) m_splitPane->navigateUp(); else goUp(); });

        m_refreshAction = toolbar->addAction(
            themedIcon(QStringLiteral("view-refresh")),
            trLocal("Odśwież", "Refresh"));
        m_refreshAction->setShortcut(QKeySequence::Refresh);
        connect(
            m_refreshAction,
            &QAction::triggered,
            this,
            [this] { refreshPane(paneContext().id); });

        m_openDolphinAction = toolbar->addAction(
            themedIcon(QStringLiteral("system-file-manager")),
            trLocal("Otwórz w Dolphinie", "Open in Dolphin"));
        connect(
            m_openDolphinAction,
            &QAction::triggered,
            this,
            [this] {
                openInDolphin(paneContext().directory);
            });

        m_addressStack = new QStackedWidget(toolbar);
        m_addressStack->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Preferred);

        m_breadcrumbFrame = new BreadcrumbFrame(m_addressStack);
        m_breadcrumbFrame->setObjectName(QStringLiteral("breadcrumbFrame"));
        m_breadcrumbFrame->setCursor(Qt::IBeamCursor);
        m_breadcrumbFrame->setToolTip(
            trLocal(
                "Kliknij bieżący segment ścieżki lub użyj Ctrl+L, aby wpisać adres",
                "Click the current path segment or press Ctrl+L to type an address"));

        connect(
            m_breadcrumbFrame,
            &BreadcrumbFrame::clicked,
            this,
            &ThisPcWindow::beginAddressEdit);
        m_breadcrumbLayout = new QHBoxLayout(m_breadcrumbFrame);
        m_breadcrumbLayout->setContentsMargins(4, 2, 4, 2);
        m_breadcrumbLayout->setSpacing(1);

        m_addressEdit = new AddressLineEdit(m_addressStack);
        m_addressEdit->setClearButtonEnabled(true);
        m_addressEdit->setPlaceholderText(
            trLocal("Wpisz ścieżkę lub adres", "Enter path or address"));
        m_addressEdit->setToolTip(
            trLocal("Ctrl+L — edytuj bieżący adres", "Ctrl+L — edit current address"));

        m_addressStack->addWidget(m_breadcrumbFrame);
        m_addressStack->addWidget(m_addressEdit);
        toolbar->addWidget(m_addressStack);

        m_searchEdit = new QLineEdit(toolbar);
        m_searchEdit->setObjectName(QStringLiteral("searchEdit"));
        m_searchEdit->setClearButtonEnabled(true);
        m_searchEdit->setPlaceholderText(
            trLocal("Szukaj w tym folderze", "Search this folder"));
        m_searchEdit->setFixedWidth(255);

        // 0.13.1: the search scope lives under the magnifier inside the
        // search box instead of consuming space between address and search.
        m_searchScopeMenu = new QMenu(m_searchEdit);
        m_searchScopeMenu->addSection(
            trLocal(
                "Zakres wyszukiwania",
                "Search scope"));

        m_searchScopeGroup =
            new QActionGroup(m_searchScopeMenu);
        m_searchScopeGroup->setExclusive(true);

        struct SearchScopeDef {
            int value;
            const char *pl;
            const char *en;
            const char *icon;
        };

        const SearchScopeDef scopes[] = {
            {0, "Bieżący folder", "Current folder", "folder"},
            {1, "Bieżący dysk", "Current drive", "drive-harddisk"},
            {2, "Ten komputer", "This PC", "computer"},
        };

        for (const SearchScopeDef &def : scopes) {
            QAction *action = m_searchScopeMenu->addAction(
                themedIcon(QString::fromLatin1(def.icon)),
                trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setData(def.value);
            m_searchScopeGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, value = def.value] {
                    m_searchScopeMode = value;
                    updateSearchControls();
                });
        }

        m_searchScopeAction =
            m_searchEdit->addAction(
                searchScopeMenuIcon(),
                QLineEdit::LeadingPosition);

        connect(
            m_searchScopeAction,
            &QAction::triggered,
            this,
            [this] {
                if (!m_searchScopeMenu
                    || !m_searchScopeAction->isEnabled()) {
                    return;
                }

                const QPoint popupPoint =
                    m_searchEdit->mapToGlobal(
                        QPoint(0, m_searchEdit->height()));
                m_searchScopeMenu->popup(popupPoint);
            });

        m_stopSearchAction =
            m_searchEdit->addAction(
                themedIcon(QStringLiteral("process-stop")),
                QLineEdit::TrailingPosition);
        m_stopSearchAction->setToolTip(
            trLocal(
                "Przerwij wyszukiwanie",
                "Stop search"));
        m_stopSearchAction->setVisible(false);

        connect(
            m_stopSearchAction,
            &QAction::triggered,
            this,
            [this] {
                cancelActiveSearch(true);
            });

        toolbar->addWidget(m_searchEdit);

        m_searchFilterButton = new QToolButton(toolbar);
        m_searchFilterButton->setObjectName(
            QStringLiteral("searchFilterButton"));
        m_searchFilterButton->setPopupMode(
            QToolButton::InstantPopup);
        m_searchFilterButton->setToolButtonStyle(
            Qt::ToolButtonIconOnly);
        m_searchFilterButton->setIcon(
            themedIcon(QStringLiteral("view-filter")));
        m_searchFilterButton->setToolTip(
            trLocal("Filtry wyszukiwania", "Search filters"));

        auto *filterMenu = new QMenu(m_searchFilterButton);

        QMenu *typeMenu =
            filterMenu->addMenu(
                trLocal("Typ", "Type"));
        m_searchTypeGroup = new QActionGroup(typeMenu);
        m_searchTypeGroup->setExclusive(true);

        struct SearchFilterDef {
            int value;
            const char *pl;
            const char *en;
        };

        const SearchFilterDef types[] = {
            {0, "Wszystkie", "All"},
            {1, "Foldery", "Folders"},
            {2, "Obrazy", "Images"},
            {3, "Dokumenty", "Documents"},
            {4, "Muzyka / audio", "Music / audio"},
            {5, "Wideo", "Video"},
            {6, "Archiwa", "Archives"},
        };

        for (const SearchFilterDef &def : types) {
            QAction *action =
                typeMenu->addAction(
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setData(def.value);
            m_searchTypeGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, value = def.value] {
                    m_searchTypeFilter = value;
                    syncCurrentSearchFilters();
                    updateSearchControls();
                    if (isSearchLocation(m_currentUrl)) {
                        renderDirectoryItems();
                    }
                });
        }

        QMenu *dateMenu =
            filterMenu->addMenu(
                trLocal("Data modyfikacji", "Date modified"));
        m_searchDateGroup = new QActionGroup(dateMenu);
        m_searchDateGroup->setExclusive(true);

        const SearchFilterDef dates[] = {
            {0, "Dowolna", "Any time"},
            {1, "Ostatnie 24 godziny", "Last 24 hours"},
            {2, "Ostatnie 7 dni", "Last 7 days"},
            {3, "Ostatnie 30 dni", "Last 30 days"},
            {4, "Ostatni rok", "Last year"},
        };

        for (const SearchFilterDef &def : dates) {
            QAction *action =
                dateMenu->addAction(
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setData(def.value);
            m_searchDateGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, value = def.value] {
                    m_searchDateFilter = value;
                    syncCurrentSearchFilters();
                    updateSearchControls();
                    if (isSearchLocation(m_currentUrl)) {
                        renderDirectoryItems();
                    }
                });
        }

        QMenu *sizeMenu =
            filterMenu->addMenu(
                trLocal("Rozmiar", "Size"));
        m_searchSizeGroup = new QActionGroup(sizeMenu);
        m_searchSizeGroup->setExclusive(true);

        const SearchFilterDef sizes[] = {
            {0, "Dowolny", "Any size"},
            {1, "Mniejszy niż 1 MiB", "Smaller than 1 MiB"},
            {2, "1–100 MiB", "1–100 MiB"},
            {3, "100 MiB–1 GiB", "100 MiB–1 GiB"},
            {4, "Większy niż 1 GiB", "Larger than 1 GiB"},
        };

        for (const SearchFilterDef &def : sizes) {
            QAction *action =
                sizeMenu->addAction(
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setData(def.value);
            m_searchSizeGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, value = def.value] {
                    m_searchSizeFilter = value;
                    syncCurrentSearchFilters();
                    updateSearchControls();
                    if (isSearchLocation(m_currentUrl)) {
                        renderDirectoryItems();
                    }
                });
        }

        filterMenu->addSeparator();

        QAction *clearFilters =
            filterMenu->addAction(
                themedIcon(QStringLiteral("edit-clear")),
                trLocal("Wyczyść filtry", "Clear filters"));

        connect(
            clearFilters,
            &QAction::triggered,
            this,
            [this] {
                m_searchTypeFilter = 0;
                m_searchDateFilter = 0;
                m_searchSizeFilter = 0;
                syncCurrentSearchFilters();
                updateSearchControls();
                if (isSearchLocation(m_currentUrl)) {
                    renderDirectoryItems();
                }
            });

        m_searchFilterButton->setMenu(filterMenu);
        toolbar->addWidget(m_searchFilterButton);

        connect(
            m_searchEdit,
            &QLineEdit::textChanged,
            this,
            [this](const QString &text) {
                if (m_contentStack
                    && m_contentStack->currentWidget()
                        == m_directoryPage
                    && !isSearchLocation(m_currentUrl)) {
                    m_searchQuery = text.trimmed();
                    renderDirectoryItems();
                } else {
                    m_searchQuery.clear();
                }
            });

        connect(
            m_searchEdit,
            &QLineEdit::returnPressed,
            this,
            &ThisPcWindow::startSearchFromUi);

        auto *findAction = new QAction(this);
        findAction->setShortcut(QKeySequence::Find);
        addAction(findAction);
        connect(
            findAction,
            &QAction::triggered,
            this,
            [this] {
                if (!m_searchEdit) {
                    return;
                }

                m_searchEdit->setFocus(
                    Qt::ShortcutFocusReason);
                m_searchEdit->selectAll();
            });

        auto *locationAction = new QAction(this);
        locationAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
        addAction(locationAction);

        connect(
            locationAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::beginAddressEdit);

        connect(
            m_addressEdit,
            &QLineEdit::returnPressed,
            this,
            [this] {
                const QUrl target =
                    urlFromUserText(m_addressEdit->text());

                m_addressStack->setCurrentWidget(m_breadcrumbFrame);

                if (target.isValid()) {
                    navigateTo(target, true);
                }
            });

        connect(
            m_addressEdit,
            &AddressLineEdit::canceled,
            this,
            [this] {
                m_addressStack->setCurrentWidget(m_breadcrumbFrame);
                setFocus();
            });

        connect(
            m_addressEdit,
            &AddressLineEdit::focusLeft,
            this,
            [this] {
                // Defer until Qt has completed the focus transition. This
                // avoids fighting with clicks on the search box or toolbar.
                QTimer::singleShot(
                    0,
                    this,
                    [this] {
                        if (m_addressEdit
                            && !m_addressEdit->hasFocus()
                            && m_addressStack->currentWidget()
                                == m_addressEdit) {
                            m_addressStack->setCurrentWidget(
                                m_breadcrumbFrame);
                        }
                    });
            });

        updateSearchControls();
    }

    void beginAddressEdit()
    {
        if (!m_addressEdit || !m_addressStack) {
            return;
        }

        m_addressEdit->setText(
            urlForDisplay(m_currentUrl));
        m_addressStack->setCurrentWidget(
            m_addressEdit);
        m_addressEdit->setFocus(
            Qt::MouseFocusReason);
        m_addressEdit->selectAll();
    }

    void setupUndoRedoManager()
    {
        if (!m_fileUndoManager) {
            updateUndoRedoActions();
            return;
        }

        connect(
            m_fileUndoManager,
            &KIO::FileUndoManager::undoAvailable,
            this,
            [this](bool) {
                updateUndoRedoActions();
            });
        connect(
            m_fileUndoManager,
            &KIO::FileUndoManager::redoAvailable,
            this,
            [this](bool) {
                updateUndoRedoActions();
            });
        connect(
            m_fileUndoManager,
            &KIO::FileUndoManager::undoTextChanged,
            this,
            [this](const QString &) {
                updateUndoRedoActions();
            });
        connect(
            m_fileUndoManager,
            &KIO::FileUndoManager::redoTextChanged,
            this,
            [this](const QString &) {
                updateUndoRedoActions();
            });
        connect(
            m_fileUndoManager,
            &KIO::FileUndoManager::jobRecordingFinished,
            this,
            [this](KIO::FileUndoManager::CommandType) {
                updateUndoRedoActions();
            });
        connect(
            m_fileUndoManager,
            &KIO::FileUndoManager::undoJobFinished,
            this,
            [this] {
                const int completedMode = m_undoRedoMode;
                m_undoRedoMode = 0;
                m_undoRedoBusy = false;

                refreshCurrent();
                if (m_splitPane && m_splitPane->isVisible()) {
                    m_splitPane->refresh();
                }
                updateFileActionStates();
                updateUndoRedoActions();

                if (completedMode == 1) {
                    statusBar()->showMessage(
                        trLocal(
                            "Cofanie zakończone",
                            "Undo completed"),
                        4000);
                } else if (completedMode == 2) {
                    statusBar()->showMessage(
                        trLocal(
                            "Ponawianie zakończone",
                            "Redo completed"),
                        4000);
                }
            });

        updateUndoRedoActions();
    }

    void updateUndoRedoActions()
    {
        const bool managerAvailable = m_fileUndoManager != nullptr;
        const bool canUndo =
            managerAvailable
            && !m_undoRedoBusy
            && m_fileUndoManager->isUndoAvailable();
        const bool canRedo =
            managerAvailable
            && !m_undoRedoBusy
            && m_fileUndoManager->isRedoAvailable();

        if (m_undoAction) {
            m_undoAction->setEnabled(canUndo);

            QString text =
                managerAvailable
                    ? m_fileUndoManager->undoText().trimmed()
                    : QString();
            if (text.isEmpty()) {
                text = trLocal("Cofnij", "Undo");
            }
            m_undoAction->setToolTip(
                QStringLiteral("%1 (%2)")
                    .arg(
                        text,
                        QStringLiteral("Ctrl+Z")));
        }

        if (m_redoAction) {
            m_redoAction->setEnabled(canRedo);

            QString text =
                managerAvailable
                    ? m_fileUndoManager->redoText().trimmed()
                    : QString();
            if (text.isEmpty()) {
                text = trLocal("Ponów", "Redo");
            }
            m_redoAction->setToolTip(
                QStringLiteral("%1 (Ctrl+Y / Ctrl+Shift+Z)")
                    .arg(text));
        }
    }

    void undoLastFileOperation()
    {
        if (!m_fileUndoManager
            || m_undoRedoBusy
            || !m_fileUndoManager->isUndoAvailable()) {
            return;
        }

        if (m_fileUndoManager->uiInterface()) {
            m_fileUndoManager->uiInterface()->setParentWidget(this);
        }

        m_undoRedoBusy = true;
        m_undoRedoMode = 1;
        updateUndoRedoActions();
        statusBar()->showMessage(
            trLocal("Cofanie operacji…", "Undoing operation…"));
        m_fileUndoManager->undo();
    }

    void redoLastFileOperation()
    {
        if (!m_fileUndoManager
            || m_undoRedoBusy
            || !m_fileUndoManager->isRedoAvailable()) {
            return;
        }

        if (m_fileUndoManager->uiInterface()) {
            m_fileUndoManager->uiInterface()->setParentWidget(this);
        }

        m_undoRedoBusy = true;
        m_undoRedoMode = 2;
        updateUndoRedoActions();
        statusBar()->showMessage(
            trLocal("Ponawianie operacji…", "Redoing operation…"));
        m_fileUndoManager->redo();
    }

    void recordCopyJobForUndo(KIO::CopyJob *job)
    {
        if (job && m_fileUndoManager) {
            m_fileUndoManager->recordCopyJob(job);
        }
    }

    void recordFileJobForUndo(
        KIO::FileUndoManager::CommandType type,
        const QList<QUrl> &sources,
        const QUrl &destination,
        KIO::Job *job)
    {
        if (job && m_fileUndoManager) {
            m_fileUndoManager->recordJob(
                type,
                sources,
                destination,
                job);
        }
    }

    void buildFileActions()
    {
        // First toolbar row: file commands. Navigation/address/search are
        // created afterwards on the row below.
        auto *toolbar =
            addToolBar(trLocal("Pliki", "Files"));

        toolbar->setObjectName(
            QStringLiteral("fileCommandToolbar"));
        addToolBar(Qt::TopToolBarArea, toolbar);
        toolbar->setMovable(false);
        toolbar->setFloatable(false);
        toolbar->setAllowedAreas(Qt::TopToolBarArea);
        toolbar->setIconSize(QSize(18, 18));
        toolbar->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);

        m_newButton = new QToolButton(toolbar);
        m_newButton->setText(trLocal("Nowy", "New"));
        m_newButton->setIcon(
            themedIcon(QStringLiteral("list-add"), QStringLiteral("folder-new")));
        m_newButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_newButton->setPopupMode(
            QToolButton::InstantPopup);

        auto *newMenu = new QMenu(m_newButton);

        m_newFolderAction = newMenu->addAction(
            themedIcon(QStringLiteral("folder-new")),
            trLocal("Folder", "Folder"));
        connect(
            m_newFolderAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::createNewFolder);

        m_newTextFileAction = newMenu->addAction(
            themedIcon(QStringLiteral("text-plain"), QStringLiteral("text-x-generic")),
            trLocal("Dokument tekstowy", "Text document"));
        connect(
            m_newTextFileAction,
            &QAction::triggered,
            this,
            [this] {
                createNewFile(
                    trLocal("Nowy dokument.txt", "New document.txt"),
                    QByteArray());
            });

        m_newEmptyFileAction = newMenu->addAction(
            themedIcon(QStringLiteral("document-new")),
            trLocal("Pusty plik…", "Empty file…"));
        connect(
            m_newEmptyFileAction,
            &QAction::triggered,
            this,
            [this] {
                createNewFile(QString(), QByteArray());
            });

        m_newButton->setMenu(newMenu);
        toolbar->addWidget(m_newButton);

        toolbar->addSeparator();

        m_cutAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-cut")),
            trLocal("Wytnij", "Cut"));
        connect(
            m_cutAction,
            &QAction::triggered,
            this,
            [this] {
                putSelectionOnClipboard(true);
            });

        m_copyAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-copy")),
            trLocal("Kopiuj", "Copy"));
        connect(
            m_copyAction,
            &QAction::triggered,
            this,
            [this] {
                putSelectionOnClipboard(false);
            });

        m_pasteAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-paste")),
            trLocal("Wklej", "Paste"));
        connect(
            m_pasteAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::pasteClipboard);

        for (QAction *action : {m_cutAction, m_copyAction, m_pasteAction}) {
            if (auto *button =
                    qobject_cast<QToolButton *>(toolbar->widgetForAction(action))) {
                button->setToolButtonStyle(Qt::ToolButtonIconOnly);
                button->setToolTip(action->text());
            }
        }

        m_trashAction = toolbar->addAction(
            themedIcon(QStringLiteral("user-trash")),
            trLocal("Do Kosza", "Trash"));
        connect(
            m_trashAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::trashSelected);

        if (auto *button =
                qobject_cast<QToolButton *>(
                    toolbar->widgetForAction(m_trashAction))) {
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setToolTip(m_trashAction->text());
        }

        toolbar->addSeparator();

        m_renameAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-rename")),
            trLocal("Zmień nazwę", "Rename"));
        connect(
            m_renameAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::renameSelected);

        toolbar->addSeparator();

        m_propertiesAction = toolbar->addAction(
            themedIcon(QStringLiteral("document-properties")),
            trLocal("Właściwości", "Properties"));
        connect(m_propertiesAction, &QAction::triggered,
                this, &ThisPcWindow::showSelectedProperties);

        m_undoAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-undo")),
            trLocal("Cofnij", "Undo"));
        m_undoAction->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Z")));
        connect(
            m_undoAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::undoLastFileOperation);

        m_redoAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-redo")),
            trLocal("Ponów", "Redo"));
        m_redoAction->setShortcuts(
            {QKeySequence(QStringLiteral("Ctrl+Y")),
             QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
        connect(
            m_redoAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::redoLastFileOperation);

        for (QAction *action : {m_undoAction, m_redoAction}) {
            if (auto *button =
                    qobject_cast<QToolButton *>(toolbar->widgetForAction(action))) {
                button->setToolButtonStyle(Qt::ToolButtonIconOnly);
                button->setToolTip(action->text());
            }
        }

        updateUndoRedoActions();

        toolbar->addSeparator();

        QSettings settings;
        m_directoryViewMode =
            std::clamp(
                settings.value(
                    QStringLiteral("directory/viewMode"),
                    0).toInt(),
                0,
                2);

        m_sortKey =
            std::clamp(
                settings.value(
                    QStringLiteral("directory/sortKey"),
                    0).toInt(),
                0,
                3);

        m_sortAscending =
            settings.value(
                QStringLiteral("directory/sortAscending"),
                true).toBool();

        m_showHiddenFiles =
            settings.value(
                QStringLiteral("directory/showHidden"),
                false).toBool();

        m_thumbnailsEnabled =
            settings.value(
                QStringLiteral("directory/thumbnails"),
                true).toBool();

        m_alwaysShowFullNames =
            settings.value(
                QStringLiteral("directory/alwaysShowFullNames"),
                false).toBool();

        m_restorePreviousSession =
            settings.value(
                QStringLiteral("session/restorePrevious"),
                true).toBool();

        m_viewButton = new QToolButton(toolbar);
        m_viewButton->setText(trLocal("Widok", "View"));
        m_viewButton->setIcon(
            themedIcon(QStringLiteral("view-list-icons")));
        m_viewButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_viewButton->setPopupMode(
            QToolButton::InstantPopup);

        auto *viewMenu = new QMenu(m_viewButton);
        auto *viewGroup = new QActionGroup(viewMenu);
        viewGroup->setExclusive(true);

        struct ViewDef {
            int mode;
            const char *pl;
            const char *en;
            const char *icon;
        };

        const ViewDef views[] = {
            {0, "Ikony", "Icons", "view-list-icons"},
            {1, "Lista", "List", "view-list-text"},
            {2, "Szczegóły", "Details", "view-list-details"},
        };

        for (const ViewDef &def : views) {
            QAction *action = viewMenu->addAction(
                themedIcon(QString::fromLatin1(def.icon)),
                trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setChecked(def.mode == m_directoryViewMode);
            action->setData(def.mode);
            viewGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, mode = def.mode] {
                    if (paneContext().id == PaneId::Split) m_splitPane->setViewMode(mode);
                    else setDirectoryViewMode(mode);
                });
        }

        viewMenu->addSeparator();

        m_showHiddenAction = viewMenu->addAction(
            themedIcon(QStringLiteral("view-hidden")),
            trLocal("Ukryte elementy", "Hidden items"));
        m_showHiddenAction->setCheckable(true);
        m_showHiddenAction->setChecked(m_showHiddenFiles);
        m_showHiddenAction->setShortcut(
            QKeySequence(Qt::CTRL | Qt::Key_H));
        addAction(m_showHiddenAction);
        connect(
            m_showHiddenAction,
            &QAction::toggled,
            this,
            &ThisPcWindow::setShowHiddenFiles);

        m_thumbnailsAction = viewMenu->addAction(
            themedIcon(QStringLiteral("view-preview")),
            trLocal("Miniatury obrazów", "Image thumbnails"));
        m_thumbnailsAction->setCheckable(true);
        m_thumbnailsAction->setChecked(m_thumbnailsEnabled);
        connect(
            m_thumbnailsAction,
            &QAction::toggled,
            this,
            &ThisPcWindow::setThumbnailsEnabled);

        viewMenu->addSeparator();

        m_restoreSessionAction = viewMenu->addAction(
            themedIcon(QStringLiteral("document-open-recent")),
            trLocal(
                "Przywracaj poprzednią sesję",
                "Restore previous session"));
        m_restoreSessionAction->setCheckable(true);
        m_restoreSessionAction->setChecked(
            m_restorePreviousSession);
        connect(
            m_restoreSessionAction,
            &QAction::toggled,
            this,
            [this](bool enabled) {
                m_restorePreviousSession = enabled;
                QSettings settings;
                settings.setValue(
                    QStringLiteral("session/restorePrevious"),
                    enabled);
            });

        m_viewButton->setMenu(viewMenu);
        toolbar->addWidget(m_viewButton);

        m_sortButton = new QToolButton(toolbar);
        m_sortButton->setText(trLocal("Sortuj", "Sort"));
        m_sortButton->setIcon(
            themedIcon(QStringLiteral("view-sort-ascending")));
        m_sortButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_sortButton->setPopupMode(
            QToolButton::InstantPopup);

        auto *sortMenu = new QMenu(m_sortButton);
        auto *sortGroup = new QActionGroup(sortMenu);
        sortGroup->setExclusive(true);

        struct SortDef {
            int key;
            const char *pl;
            const char *en;
        };

        const SortDef sorts[] = {
            {0, "Nazwa", "Name"},
            {1, "Typ", "Type"},
            {2, "Rozmiar", "Size"},
            {3, "Data modyfikacji", "Date modified"},
        };

        for (const SortDef &def : sorts) {
            QAction *action = sortMenu->addAction(
                trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setChecked(def.key == m_sortKey);
            action->setData(def.key);
            sortGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, key = def.key] {
                    if (paneContext().id == PaneId::Split) m_splitPane->setSortState(key, m_splitPane->sortAscending());
                    else setSortKey(key);
                });
        }

        sortMenu->addSeparator();

        auto *directionGroup =
            new QActionGroup(sortMenu);
        directionGroup->setExclusive(true);

        QAction *ascending =
            sortMenu->addAction(
                themedIcon(QStringLiteral("view-sort-ascending")),
                trLocal("Rosnąco", "Ascending"));
        ascending->setCheckable(true);
        ascending->setChecked(m_sortAscending);
        directionGroup->addAction(ascending);

        QAction *descending =
            sortMenu->addAction(
                themedIcon(QStringLiteral("view-sort-descending")),
                trLocal("Malejąco", "Descending"));
        descending->setCheckable(true);
        descending->setChecked(!m_sortAscending);
        directionGroup->addAction(descending);

        connect(
            ascending,
            &QAction::triggered,
            this,
            [this] {
                if (paneContext().id == PaneId::Split) m_splitPane->setSortState(m_splitPane->sortKey(), true);
                else setSortAscending(true);
            });

        connect(
            descending,
            &QAction::triggered,
            this,
            [this] {
                if (paneContext().id == PaneId::Split) m_splitPane->setSortState(m_splitPane->sortKey(), false);
                else setSortAscending(false);
            });

        m_sortButton->setMenu(sortMenu);
        toolbar->addWidget(m_sortButton);

        toolbar->addSeparator();

        m_fullNamesAction = toolbar->addAction(
            themedIcon(
                QStringLiteral("format-text-underline"),
                QStringLiteral("view-list-text")),
            trLocal("Pełne nazwy", "Full names"));
        m_fullNamesAction->setCheckable(true);
        m_fullNamesAction->setChecked(
            m_alwaysShowFullNames);
        m_fullNamesAction->setToolTip(
            trLocal(
                "Zawsze pokazuj pełne nazwy plików i folderów. Bez tej opcji pełna nazwa rozwija się po zaznaczeniu.",
                "Always show full file and folder names. When disabled, a selected item expands to show its full name."));
        connect(
            m_fullNamesAction,
            &QAction::toggled,
            this,
            &ThisPcWindow::setAlwaysShowFullNames);

        toolbar->addSeparator();

        m_splitViewAction = toolbar->addAction(
            themedIcon(QStringLiteral("view-split-left-right"), QStringLiteral("view-list-details")),
            trLocal("Podziel widok", "Split view"));
        m_splitViewAction->setCheckable(true);
        m_splitViewAction->setShortcut(QKeySequence(Qt::Key_F3));
        m_splitViewAction->setToolTip(
            trLocal("Podziel widok na dwa panele (F3)", "Split the view into two panes (F3)"));
        connect(
            m_splitViewAction,
            &QAction::toggled,
            this,
            &ThisPcWindow::setSplitViewEnabled);

        // 0.15.1: compact operation history lives at the far-right edge of
        // the command bar and opens as a Brave-like popup.
        buildOperationManager(toolbar);

        // Keyboard shortcuts are scoped to the directory page so Ctrl+C/X/V,
        // F2 and Delete do not steal keystrokes while editing Ctrl+L.
        updateFileActionStates();
    }

    void buildCentralUi()
    {
        auto *central = new QWidget(this);
        auto *centralLayout = new QHBoxLayout(central);
        centralLayout->setContentsMargins(0, 0, 0, 0);
        centralLayout->setSpacing(0);
        setCentralWidget(central);

        m_sidebar = new QFrame(central);
        m_sidebar->setObjectName(QStringLiteral("sidebar"));
        m_sidebar->setMinimumWidth(205);
        m_sidebar->setMaximumWidth(235);
        m_sidebar->setSizePolicy(
            QSizePolicy::Fixed,
            QSizePolicy::Expanding);

        m_sidebarLayout = new QVBoxLayout(m_sidebar);
        m_sidebarLayout->setContentsMargins(6, 5, 6, 8);
        m_sidebarLayout->setSpacing(0);

        centralLayout->addWidget(m_sidebar);

        auto *rightPane = new QWidget(central);
        auto *rightLayout = new QVBoxLayout(rightPane);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(0);

        m_tabStrip = new QFrame(rightPane);
        m_tabStrip->setObjectName(QStringLiteral("tabStrip"));
        auto *tabLayout = new QHBoxLayout(m_tabStrip);
        tabLayout->setContentsMargins(4, 0, 2, 0);
        tabLayout->setSpacing(0);

        m_tabBar = new ExplorerTabBar(m_tabStrip);
        m_tabBar->setDropDirectoryResolver([this](int index) {
            return tabDropDirectory(index);
        });
        connect(m_tabBar, &ExplorerTabBar::urlsDropped,
                this, &ThisPcWindow::handleDroppedUrls);
        m_tabBar->setObjectName(QStringLiteral("explorerTabs"));
        m_tabBar->setTabsClosable(true);
        m_tabBar->setMovable(true);
        m_tabBar->setDocumentMode(true);
        m_tabBar->setElideMode(Qt::ElideRight);
        m_tabBar->setExpanding(false);
        m_tabBar->setUsesScrollButtons(true);
        m_tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
        tabLayout->addWidget(m_tabBar, 1);

        m_newTabButton = new QToolButton(m_tabStrip);
        m_newTabButton->setObjectName(QStringLiteral("newTabButton"));
        m_newTabButton->setIcon(themedIcon(QStringLiteral("list-add")));
        m_newTabButton->setToolTip(trLocal(
            "Nowa karta (Ctrl+T)",
            "New tab (Ctrl+T)"));
        tabLayout->addWidget(m_newTabButton, 0, Qt::AlignTop);

        connect(
            m_newTabButton,
            &QToolButton::clicked,
            this,
            [this] { createNewTab(kThisPcUrl, true); });

        connect(
            m_tabBar,
            &QTabBar::currentChanged,
            this,
            [this](int index) {
                if (!m_tabChangeInProgress) {
                    switchToTab(index);
                }
            });

        connect(
            m_tabBar,
            &QTabBar::tabCloseRequested,
            this,
            &ThisPcWindow::closeTab);

        connect(
            m_tabBar,
            &QTabBar::tabMoved,
            this,
            [this](int from, int to) {
                if (from < 0 || to < 0
                    || from >= m_tabs.size()
                    || to >= m_tabs.size()) {
                    return;
                }

                syncActiveTabState();
                m_tabs.move(from, to);
                m_activeTab = m_tabBar->currentIndex();
            });

        connect(
            m_tabBar,
            &QWidget::customContextMenuRequested,
            this,
            &ThisPcWindow::showTabContextMenu);

        m_contentSplitter = new QSplitter(Qt::Horizontal, rightPane);
        m_contentSplitter->setObjectName(QStringLiteral("contentSplitter"));
        m_contentSplitter->setChildrenCollapsible(false);
        m_contentSplitter->setHandleWidth(1);

        m_primaryPane = new QWidget(m_contentSplitter);
        m_primaryPane->setObjectName(
            QStringLiteral("primaryBrowserPane"));
        m_primaryPane->setProperty("active", true);

        auto *primaryLayout = new QVBoxLayout(m_primaryPane);
        primaryLayout->setContentsMargins(0, 0, 0, 0);
        primaryLayout->setSpacing(0);

        m_contentStack = new QStackedWidget(m_primaryPane);
        primaryLayout->addWidget(m_contentStack, 1);
        m_contentSplitter->addWidget(m_primaryPane);

        m_splitPane = new SplitBrowserPane(m_contentSplitter);
        m_splitPane->setAlwaysShowFullNames(
            m_alwaysShowFullNames);
        m_contentSplitter->addWidget(m_splitPane);
        m_splitPane->hide();

        QSettings splitSettings;
        const QByteArray storedSplitState =
            splitSettings.value(
                QStringLiteral("split/state"))
                .toByteArray();
        if (storedSplitState.isEmpty()
            || !m_contentSplitter->restoreState(storedSplitState)) {
            m_contentSplitter->setSizes({650, 450});
        }

        connect(
            m_contentSplitter,
            &QSplitter::splitterMoved,
            this,
            [this] {
                QSettings settings;
                settings.setValue(
                    QStringLiteral("split/state"),
                    m_contentSplitter->saveState());
            });

        connect(
            m_splitPane,
            &SplitBrowserPane::closeRequested,
            this,
            [this] { setSplitViewEnabled(false); });
        connect(
            m_splitPane,
            &SplitBrowserPane::swapRequested,
            this,
            &ThisPcWindow::swapSplitPanes);
        connect(
            m_splitPane,
            &SplitBrowserPane::openInPrimaryRequested,
            this,
            [this](const QUrl &url) { navigateTo(url, true); });
        connect(
            m_splitPane,
            &SplitBrowserPane::openInNewTabRequested,
            this,
            [this](const QUrl &url) { createNewTab(url, true); });
        connect(
            m_splitPane,
            &SplitBrowserPane::openInNewWindowRequested,
            this,
            &ThisPcWindow::openInNewWindow);
        connect(
            m_splitPane,
            &SplitBrowserPane::urlsDropped,
            this,
            &ThisPcWindow::handleDroppedUrls);

        connect(
            m_splitPane,
            &SplitBrowserPane::stateChanged,
            this,
            [this] {
                if (!m_tabRestoreInProgress) {
                    syncActiveTabState();
                }
                updateFileActionStates();
            });

        connect(m_splitPane, &SplitBrowserPane::selectionChanged,
                this, &ThisPcWindow::updateFileActionStates);
        connect(m_splitPane, &SplitBrowserPane::contextMenuRequested,
                this, [this](bool details, const QPoint &pos) {
                    showPaneContextMenu(PaneId::Split, details, pos);
                });

        connect(
            qApp,
            &QApplication::focusChanged,
            this,
            [this](QWidget *, QWidget *now) {
                if (!now
                    || !m_primaryPane
                    || !m_splitPane) {
                    return;
                }

                const bool splitActive =
                    m_splitPane->isVisible()
                    && (now == m_splitPane
                        || m_splitPane->isAncestorOf(now));

                const bool primaryActive =
                    now == m_primaryPane
                    || m_primaryPane->isAncestorOf(now);

                if (!splitActive && !primaryActive) {
                    return;
                }

                setActivePane(splitActive ? PaneId::Split : PaneId::Primary);
            });

        rightLayout->addWidget(m_tabStrip);
        rightLayout->addWidget(m_contentSplitter, 1);
        centralLayout->addWidget(rightPane, 1);

        buildHomePage();
        buildDirectoryPage();
        buildTabShortcuts();
        buildSplitShortcuts();

        statusBar()->setSizeGripEnabled(true);

        m_versionLabel = new QLabel(
            QStringLiteral("v0.19.0.3"),
            this);
        m_versionLabel->setObjectName(
            QStringLiteral("versionLabel"));
        m_versionLabel->setToolTip(
            trLocal(
                "Wersja thispc-view 0.16.0",
                "thispc-view version 0.16.0"));
        statusBar()->addPermanentWidget(m_versionLabel);
    }

    void buildOperationManager(QToolBar *toolbar)
    {
        if (!toolbar) {
            return;
        }

        auto *spacer = new QWidget(toolbar);
        spacer->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Preferred);
        toolbar->addWidget(spacer);

        m_operationButton = new QToolButton(toolbar);
        m_operationButton->setObjectName(
            QStringLiteral("operationButton"));
        m_operationButton->setIcon(
            themedIcon(
                QStringLiteral("view-task"),
                QStringLiteral("folder-sync")));
        m_operationButton->setToolButtonStyle(
            Qt::ToolButtonIconOnly);
        m_operationButton->setFixedSize(34, 32);
        m_operationButton->setToolTip(
            trLocal("Operacje plikowe", "File operations"));
        toolbar->addWidget(m_operationButton);

        m_operationBadgeLabel = new QLabel(m_operationButton);
        m_operationBadgeLabel->setObjectName(
            QStringLiteral("operationBadge"));
        m_operationBadgeLabel->setAlignment(Qt::AlignCenter);
        m_operationBadgeLabel->setAttribute(
            Qt::WA_TransparentForMouseEvents);
        m_operationBadgeLabel->setFixedSize(18, 16);
        m_operationBadgeLabel->move(16, -1);
        m_operationBadgeLabel->hide();

        // A custom Qt::Popup is used instead of QMenu/QWidgetAction.
        // QMenu caches action geometry aggressively and kept the file
        // operation list at roughly two rows even after its child widget
        // became taller. With our own popup we fully control its height.
        m_operationPopup = new QFrame(
            this,
            Qt::Popup | Qt::FramelessWindowHint);
        m_operationPopup->setObjectName(
            QStringLiteral("operationPopup"));
        m_operationPopup->setFixedWidth(410);
        m_operationPopup->setMaximumHeight(720);
        m_operationPopup->hide();

        auto *popupLayout = new QVBoxLayout(m_operationPopup);
        popupLayout->setContentsMargins(0, 0, 0, 0);
        popupLayout->setSpacing(0);

        auto *header = new QFrame(m_operationPopup);
        header->setObjectName(
            QStringLiteral("operationPopupHeader"));
        auto *headerLayout = new QVBoxLayout(header);
        headerLayout->setContentsMargins(12, 10, 12, 8);
        headerLayout->setSpacing(5);

        auto *titleRow = new QHBoxLayout;
        titleRow->setContentsMargins(0, 0, 0, 0);
        titleRow->setSpacing(8);

        auto *title = new QLabel(
            trLocal("Operacje", "Operations"),
            header);
        title->setObjectName(
            QStringLiteral("operationPopupTitle"));
        titleRow->addWidget(title);

        titleRow->addStretch(1);

        m_operationSummaryLabel = new QLabel(header);
        m_operationSummaryLabel->setObjectName(
            QStringLiteral("operationSummaryLabel"));
        titleRow->addWidget(m_operationSummaryLabel);

        headerLayout->addLayout(titleRow);

        m_operationOverallProgress = new QProgressBar(header);
        m_operationOverallProgress->setObjectName(
            QStringLiteral("operationOverallProgress"));
        m_operationOverallProgress->setRange(0, 100);
        m_operationOverallProgress->setValue(0);
        m_operationOverallProgress->setTextVisible(false);
        m_operationOverallProgress->hide();
        headerLayout->addWidget(m_operationOverallProgress);

        popupLayout->addWidget(header);

        auto *body = new QWidget(m_operationPopup);
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(8, 8, 8, 8);
        bodyLayout->setSpacing(0);

        m_operationEmptyLabel = new QLabel(
            trLocal(
                "Brak operacji plikowych.",
                "No file operations."),
            body);
        m_operationEmptyLabel->setObjectName(
            QStringLiteral("operationEmptyLabel"));
        m_operationEmptyLabel->setAlignment(Qt::AlignCenter);
        m_operationEmptyLabel->setMinimumHeight(90);
        bodyLayout->addWidget(m_operationEmptyLabel);

        auto *scroll = new QScrollArea(body);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(
            Qt::ScrollBarAlwaysOff);
        scroll->setMinimumHeight(100);
        scroll->setMaximumHeight(560);
        scroll->setVerticalScrollBarPolicy(
            Qt::ScrollBarAsNeeded);

        m_operationListWidget = new QWidget(scroll);
        m_operationListLayout = new QVBoxLayout(
            m_operationListWidget);
        m_operationListLayout->setContentsMargins(0, 0, 0, 0);
        m_operationListLayout->setSpacing(6);
        m_operationListLayout->addStretch(1);
        scroll->setWidget(m_operationListWidget);
        bodyLayout->addWidget(scroll);

        m_operationScrollArea = scroll;
        popupLayout->addWidget(body);

        auto *footer = new QFrame(m_operationPopup);
        footer->setObjectName(
            QStringLiteral("operationPopupFooter"));
        m_operationFooter = footer;
        auto *footerLayout = new QHBoxLayout(footer);
        footerLayout->setContentsMargins(8, 6, 8, 6);
        footerLayout->setSpacing(6);

        m_cancelAllOperationsButton = new QToolButton(footer);
        m_cancelAllOperationsButton->setObjectName(
            QStringLiteral("operationFooterButton"));
        m_cancelAllOperationsButton->setIcon(
            themedIcon(QStringLiteral("process-stop")));
        m_cancelAllOperationsButton->setText(
            trLocal("Anuluj wszystkie", "Cancel all"));
        m_cancelAllOperationsButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_cancelAllOperationsButton->hide();
        footerLayout->addWidget(m_cancelAllOperationsButton);

        footerLayout->addStretch(1);

        m_clearFinishedOperationsButton =
            new QToolButton(footer);
        m_clearFinishedOperationsButton->setObjectName(
            QStringLiteral("operationFooterButton"));
        m_clearFinishedOperationsButton->setIcon(
            themedIcon(
                QStringLiteral("edit-clear-list"),
                QStringLiteral("edit-clear")));
        m_clearFinishedOperationsButton->setText(
            trLocal(
                "Wyczyść zakończone",
                "Clear completed"));
        m_clearFinishedOperationsButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_clearFinishedOperationsButton->hide();
        footerLayout->addWidget(
            m_clearFinishedOperationsButton);

        popupLayout->addWidget(footer);

        connect(
            m_operationButton,
            &QToolButton::clicked,
            this,
            [this] {
                if (!m_operationPopup) {
                    return;
                }

                if (m_operationPopup->isVisible()) {
                    m_operationPopup->hide();
                    return;
                }

                updateOperationPopupHeight();
                positionOperationPopup();
                m_operationPopup->show();
                m_operationPopup->raise();
            });

        connect(
            m_cancelAllOperationsButton,
            &QToolButton::clicked,
            this,
            &ThisPcWindow::cancelAllFileOperations);

        connect(
            m_clearFinishedOperationsButton,
            &QToolButton::clicked,
            this,
            &ThisPcWindow::clearFinishedFileOperations);

        updateFileOperationSummary();
    }

    void positionOperationPopup()
    {
        if (!m_operationPopup || !m_operationButton) {
            return;
        }

        const QPoint buttonTopLeft =
            m_operationButton->mapToGlobal(QPoint(0, 0));

        // Keep only a small part of the popup over the browser window.
        // When there is free desktop space on the right, most of the
        // operation history floats outside the main window, Brave-style.
        constexpr int windowOverlap = 96;
        const int windowRight =
            mapToGlobal(QPoint(width(), 0)).x();

        QPoint popupPos(
            windowRight - windowOverlap,
            buttonTopLeft.y()
                + m_operationButton->height()
                + 4);

        QScreen *screen = QGuiApplication::screenAt(buttonTopLeft);
        if (!screen) {
            screen = m_operationButton->screen();
        }

        if (screen) {
            const QRect available = screen->availableGeometry();

            if (popupPos.x() < available.left() + 4) {
                popupPos.setX(available.left() + 4);
            }
            if (popupPos.x() + m_operationPopup->width()
                > available.right() - 4) {
                popupPos.setX(
                    available.right()
                    - m_operationPopup->width()
                    - 4);
            }

            if (popupPos.y() + m_operationPopup->height()
                > available.bottom() - 4) {
                const int aboveY =
                    buttonTopLeft.y()
                    - m_operationPopup->height()
                    - 4;
                popupPos.setY(
                    qMax(available.top() + 4, aboveY));
            }
        }

        m_operationPopup->move(popupPos);
    }

    void updateOperationPopupHeight()
    {
        if (!m_operationPopup
            || !m_operationScrollArea
            || !m_operationListWidget
            || !m_operationListLayout) {
            return;
        }

        constexpr int minimumListHeight = 100;
        constexpr int maximumListHeight = 560;
        constexpr int minimumRowHeight = 72;

        if (m_fileOperationItems.isEmpty()) {
            m_operationListWidget->setMinimumHeight(0);
            m_operationListWidget->setMaximumHeight(QWIDGETSIZE_MAX);
            m_operationScrollArea->setFixedHeight(
                minimumListHeight);
            m_operationScrollArea->setVerticalScrollBarPolicy(
                Qt::ScrollBarAlwaysOff);
        } else {
            m_operationListLayout->activate();

            const QMargins margins =
                m_operationListLayout->contentsMargins();
            int contentHeight = margins.top() + margins.bottom();
            int visibleRows = 0;

            for (FileOperationItem *item : m_fileOperationItems) {
                if (!item || !item->row || item->row->isHidden()) {
                    continue;
                }

                if (item->row->layout()) {
                    item->row->layout()->activate();
                }
                item->row->ensurePolished();

                const int rowHeight = qMax(
                    minimumRowHeight,
                    qMax(
                        item->row->sizeHint().height(),
                        item->row->minimumSizeHint().height()));

                if (visibleRows > 0) {
                    contentHeight += m_operationListLayout->spacing();
                }
                contentHeight += rowHeight;
                ++visibleRows;
            }

            contentHeight += 4;
            const int desiredHeight = qBound(
                minimumListHeight,
                contentHeight,
                maximumListHeight);

            // Keep the contents at their natural full height. The
            // viewport follows it until 560 px and then becomes
            // scrollable. This is independent from QMenu geometry.
            m_operationListWidget->setMinimumHeight(contentHeight);
            m_operationListWidget->setMaximumHeight(contentHeight);
            m_operationScrollArea->setFixedHeight(desiredHeight);
            m_operationScrollArea->setVerticalScrollBarPolicy(
                contentHeight > maximumListHeight
                    ? Qt::ScrollBarAsNeeded
                    : Qt::ScrollBarAlwaysOff);
        }

        if (m_operationPopup->layout()) {
            m_operationPopup->layout()->activate();
        }

        // The popup is a real top-level Qt::Popup, so force the final
        // geometry instead of relying on cached menu action geometry.
        const int preferredHeight = qMin(
            720,
            m_operationPopup->sizeHint().height());
        m_operationPopup->resize(410, preferredHeight);
        m_operationPopup->updateGeometry();

        if (m_operationPopup->isVisible()) {
            positionOperationPopup();
        }
    }

    QString operationTransferText(
        const FileOperationItem *item) const
    {
        if (!item) {
            return {};
        }

        QString transfer;

        if (item->totalBytes > 0) {
            transfer = QStringLiteral("%1 / %2")
                .arg(
                    formatFileSize(
                        static_cast<qint64>(item->processedBytes),
                        false),
                    formatFileSize(
                        static_cast<qint64>(item->totalBytes),
                        false));
        } else if (item->processedBytes > 0) {
            transfer = formatFileSize(
                static_cast<qint64>(item->processedBytes),
                false);
        }

        if (item->speedBytesPerSecond > 0) {
            const QString speed =
                formatFileSize(
                    static_cast<qint64>(
                        item->speedBytesPerSecond),
                    false)
                + QStringLiteral("/s");
            transfer = transfer.isEmpty()
                ? speed
                : transfer + QStringLiteral("  •  ") + speed;
        }

        return transfer;
    }

    void updateFileOperationDetails(
        FileOperationItem *item)
    {
        if (!item || !item->detailsLabel) {
            return;
        }

        QString details = item->contextText;
        const QString transfer = operationTransferText(item);

        if (!transfer.isEmpty()) {
            details = details.isEmpty()
                ? transfer
                : details + QStringLiteral("  •  ") + transfer;
        }

        if (details.isEmpty() && !item->finished) {
            details = trLocal(
                "Oczekiwanie na informacje o postępie…",
                "Waiting for progress information…");
        }

        item->detailsLabel->setText(details);
        updateOperationPopupHeight();
    }

    int activeFileOperationCount() const
    {
        int count = 0;
        for (const FileOperationItem *item : m_fileOperationItems) {
            if (item && !item->finished) {
                ++count;
            }
        }
        return count;
    }

    void updateFileOperationSummary()
    {
        if (!m_operationButton || !m_operationPopup) {
            return;
        }

        int active = 0;
        int completed = 0;
        int sumPercent = 0;

        for (const FileOperationItem *item : m_fileOperationItems) {
            if (!item) {
                continue;
            }

            if (item->finished) {
                ++completed;
            } else {
                ++active;
                sumPercent += static_cast<int>(item->percent);
            }
        }

        const bool empty = m_fileOperationItems.isEmpty();

        if (m_operationEmptyLabel) {
            m_operationEmptyLabel->setVisible(empty);
        }
        if (m_operationScrollArea) {
            m_operationScrollArea->setVisible(!empty);
        }

        updateOperationPopupHeight();

        if (m_operationSummaryLabel) {
            if (empty) {
                m_operationSummaryLabel->setText(
                    trLocal("Brak historii", "No history"));
            } else if (active > 0) {
                m_operationSummaryLabel->setText(
                    isPolish()
                        ? QStringLiteral("%1 aktywne • %2 zakończone")
                            .arg(active)
                            .arg(completed)
                        : QStringLiteral("%1 active • %2 completed")
                            .arg(active)
                            .arg(completed));
            } else {
                m_operationSummaryLabel->setText(
                    isPolish()
                        ? QStringLiteral("Zakończone: %1")
                            .arg(completed)
                        : QStringLiteral("Completed: %1")
                            .arg(completed));
            }
        }

        if (m_operationOverallProgress) {
            m_operationOverallProgress->setVisible(active > 0);
            m_operationOverallProgress->setValue(
                active > 0 ? sumPercent / active : 0);
        }

        if (m_cancelAllOperationsButton) {
            m_cancelAllOperationsButton->setVisible(active > 0);
        }
        if (m_clearFinishedOperationsButton) {
            m_clearFinishedOperationsButton->setVisible(completed > 0);
        }
        if (m_operationFooter) {
            m_operationFooter->setVisible(active > 0 || completed > 0);
        }

        if (m_operationBadgeLabel) {
            if (active > 0) {
                m_operationBadgeLabel->setText(
                    active > 9
                        ? QStringLiteral("9+")
                        : QString::number(active));
                m_operationBadgeLabel->show();
                m_operationBadgeLabel->raise();
            } else {
                m_operationBadgeLabel->hide();
            }
        }

        const bool wasActive =
            m_operationButton->property("active").toBool();
        const bool isActive = active > 0;
        if (wasActive != isActive) {
            m_operationButton->setProperty("active", isActive);
            m_operationButton->style()->unpolish(m_operationButton);
            m_operationButton->style()->polish(m_operationButton);
            m_operationButton->update();
        }

        m_operationButton->setIcon(
            themedIcon(
                isActive
                    ? QStringLiteral("folder-sync")
                    : QStringLiteral("view-task"),
                QStringLiteral("system-run")));

        if (empty) {
            m_operationButton->setToolTip(
                trLocal(
                    "Operacje plikowe — brak historii",
                    "File operations — no history"));
        } else if (active > 0) {
            m_operationButton->setToolTip(
                isPolish()
                    ? QStringLiteral(
                        "Operacje plikowe — %1 aktywne, %2 zakończone")
                        .arg(active)
                        .arg(completed)
                    : QStringLiteral(
                        "File operations — %1 active, %2 completed")
                        .arg(active)
                        .arg(completed));
        } else {
            m_operationButton->setToolTip(
                isPolish()
                    ? QStringLiteral(
                        "Operacje plikowe — zakończone: %1")
                        .arg(completed)
                    : QStringLiteral(
                        "File operations — completed: %1")
                        .arg(completed));
        }

        // Footer/header visibility also changes the total popup size.
        // Recalculate once more after those states have been updated.
        updateOperationPopupHeight();

        if (m_operationPopup->isVisible()) {
            QTimer::singleShot(
                0,
                this,
                [this] {
                    if (m_operationPopup
                        && m_operationPopup->isVisible()) {
                        updateOperationPopupHeight();
                    }
                });
        }
    }

    FileOperationItem *trackFileOperation(
        KJob *job,
        const QString &operationTitle)
    {
        if (!job || !m_operationListLayout) {
            return nullptr;
        }

        auto *item = new FileOperationItem;
        item->job = job;
        item->percent = job->percent();

        item->row = new QFrame(m_operationListWidget);
        item->row->setObjectName(QStringLiteral("operationRow"));
        auto *rowLayout = new QHBoxLayout(item->row);
        rowLayout->setContentsMargins(9, 7, 7, 7);
        rowLayout->setSpacing(9);

        auto *icon = new QLabel(item->row);
        icon->setPixmap(
            themedIcon(
                QStringLiteral("folder-sync"),
                QStringLiteral("system-run"))
                .pixmap(24, 24));
        icon->setFixedSize(28, 28);
        icon->setAlignment(Qt::AlignCenter);
        rowLayout->addWidget(icon, 0, Qt::AlignTop);

        auto *textWidget = new QWidget(item->row);
        auto *textLayout = new QVBoxLayout(textWidget);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(3);

        auto *topLine = new QHBoxLayout;
        topLine->setContentsMargins(0, 0, 0, 0);
        topLine->setSpacing(7);

        item->titleLabel = new QLabel(
            operationTitle.isEmpty()
                ? trLocal("Operacja plikowa", "File operation")
                : operationTitle,
            textWidget);
        item->titleLabel->setObjectName(
            QStringLiteral("operationTitleLabel"));
        topLine->addWidget(item->titleLabel, 1);

        item->stateLabel = new QLabel(
            trLocal("W toku", "Running"),
            textWidget);
        item->stateLabel->setObjectName(
            QStringLiteral("operationDetailsLabel"));
        topLine->addWidget(item->stateLabel);
        textLayout->addLayout(topLine);

        item->detailsLabel = new QLabel(textWidget);
        item->detailsLabel->setObjectName(
            QStringLiteral("operationDetailsLabel"));
        item->detailsLabel->setTextInteractionFlags(
            Qt::TextSelectableByMouse);
        item->detailsLabel->setWordWrap(true);
        textLayout->addWidget(item->detailsLabel);

        item->progressBar = new QProgressBar(textWidget);
        item->progressBar->setObjectName(
            QStringLiteral("operationProgress"));
        item->progressBar->setRange(0, 100);
        item->progressBar->setValue(
            static_cast<int>(item->percent));
        item->progressBar->setTextVisible(false);
        textLayout->addWidget(item->progressBar);

        rowLayout->addWidget(textWidget, 1);

        item->cancelButton = new QToolButton(item->row);
        item->cancelButton->setIcon(
            themedIcon(QStringLiteral("process-stop")));
        item->cancelButton->setToolTip(
            trLocal("Anuluj operację", "Cancel operation"));
        const bool killable =
            job->capabilities().testFlag(KJob::Killable);
        item->cancelButton->setEnabled(killable);
        if (!killable) {
            item->cancelButton->setToolTip(
                trLocal(
                    "Ta operacja nie obsługuje anulowania",
                    "This operation cannot be cancelled"));
        }
        rowLayout->addWidget(item->cancelButton, 0, Qt::AlignTop);

        m_operationListLayout->insertWidget(
            qMax(0, m_operationListLayout->count() - 1),
            item->row);

        m_fileOperationItems.push_back(item);
        m_fileOperationByJob.insert(job, item);

        connect(
            item->cancelButton,
            &QToolButton::clicked,
            this,
            [this, item] {
                if (!item || item->finished || !item->job) {
                    return;
                }

                item->cancelRequested = true;
                item->stateLabel->setText(
                    trLocal("Anulowanie…", "Cancelling…"));
                item->cancelButton->setEnabled(false);

                if (!item->job->kill(KJob::EmitResult)) {
                    item->cancelRequested = false;
                    item->stateLabel->setText(
                        trLocal("W toku", "Running"));
                    item->cancelButton->setEnabled(true);
                    statusBar()->showMessage(
                        trLocal(
                            "Nie udało się anulować tej operacji.",
                            "Could not cancel this operation."),
                        4000);
                }
            });

        connect(
            job,
            &KJob::percentChanged,
            this,
            [this, item](KJob *, unsigned long percent) {
                if (!item || item->finished) {
                    return;
                }
                item->percent = qMin<unsigned long>(percent, 100UL);
                item->progressBar->setValue(
                    static_cast<int>(item->percent));
                updateFileOperationSummary();
            });

        connect(
            job,
            &KJob::processedAmountChanged,
            this,
            [this, item](
                KJob *,
                KJob::Unit unit,
                qulonglong amount) {
                if (!item || item->finished || unit != KJob::Bytes) {
                    return;
                }
                item->processedBytes = amount;
                updateFileOperationDetails(item);
            });

        connect(
            job,
            &KJob::totalAmountChanged,
            this,
            [this, item](
                KJob *,
                KJob::Unit unit,
                qulonglong amount) {
                if (!item || item->finished || unit != KJob::Bytes) {
                    return;
                }
                item->totalBytes = amount;
                updateFileOperationDetails(item);
            });

        connect(
            job,
            &KJob::speed,
            this,
            [this, item](KJob *, unsigned long speed) {
                if (!item || item->finished) {
                    return;
                }
                item->speedBytesPerSecond = speed;
                updateFileOperationDetails(item);
            });

        connect(
            job,
            &KJob::description,
            this,
            [this, item](
                KJob *,
                const QString &,
                const QPair<QString, QString> &field1,
                const QPair<QString, QString> &field2) {
                if (!item || item->finished) {
                    return;
                }

                QString context;
                if (!field1.second.isEmpty()) {
                    context = field1.first.isEmpty()
                        ? field1.second
                        : QStringLiteral("%1: %2")
                            .arg(field1.first, field1.second);
                }
                if (!field2.second.isEmpty()) {
                    const QString second = field2.first.isEmpty()
                        ? field2.second
                        : QStringLiteral("%1: %2")
                            .arg(field2.first, field2.second);
                    context = context.isEmpty()
                        ? second
                        : context + QStringLiteral("  •  ") + second;
                }

                item->contextText = context;
                updateFileOperationDetails(item);
            });

        updateFileOperationDetails(item);
        updateFileOperationSummary();
        return item;
    }

    void finishFileOperation(
        KJob *job,
        bool success,
        bool cancelled,
        const QString &errorText)
    {
        FileOperationItem *item =
            m_fileOperationByJob.value(job, nullptr);
        if (!item) {
            return;
        }

        item->finished = true;
        item->failed = !success && !cancelled;
        item->speedBytesPerSecond = 0;
        item->job.clear();
        m_fileOperationByJob.remove(job);

        if (item->cancelButton) {
            item->cancelButton->hide();
        }

        if (success) {
            item->percent = 100;
            if (item->progressBar) {
                item->progressBar->setValue(100);
            }
            item->stateLabel->setText(
                trLocal("Zakończono", "Completed"));
        } else if (cancelled) {
            item->stateLabel->setText(
                trLocal("Anulowano", "Cancelled"));
        } else {
            item->stateLabel->setText(
                trLocal("Błąd", "Failed"));
            if (!errorText.isEmpty()) {
                item->contextText = errorText;
            }
        }

        updateFileOperationDetails(item);
        updateFileOperationSummary();
    }

    void cancelAllFileOperations()
    {
        const QList<FileOperationItem *> items = m_fileOperationItems;
        for (FileOperationItem *item : items) {
            if (!item || item->finished || !item->job) {
                continue;
            }
            if (!item->job->capabilities().testFlag(KJob::Killable)) {
                continue;
            }

            item->cancelRequested = true;
            item->stateLabel->setText(
                trLocal("Anulowanie…", "Cancelling…"));
            if (item->cancelButton) {
                item->cancelButton->setEnabled(false);
            }
            if (!item->job->kill(KJob::EmitResult)) {
                item->cancelRequested = false;
                item->stateLabel->setText(
                    trLocal("W toku", "Running"));
                if (item->cancelButton) {
                    item->cancelButton->setEnabled(true);
                }
            }
        }
    }

    void clearFinishedFileOperations()
    {
        for (int index = m_fileOperationItems.size() - 1;
             index >= 0;
             --index) {
            FileOperationItem *item = m_fileOperationItems.at(index);
            if (!item || !item->finished) {
                continue;
            }

            m_fileOperationItems.removeAt(index);
            if (item->row) {
                delete item->row;
            }
            delete item;
        }

        updateFileOperationSummary();
    }

    void buildHomePage()
    {
        auto *scroll = new QScrollArea(m_contentStack);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        auto *page = new QWidget(scroll);
        scroll->setWidget(page);

        auto *mainLayout = new QVBoxLayout(page);
        mainLayout->setContentsMargins(20, 18, 20, 22);
        mainLayout->setSpacing(10);

        auto *header =
            new QLabel(trLocal("Ten komputer", "This PC"), page);

        QFont headerFont = header->font();
        headerFont.setPointSize(headerFont.pointSize() + 5);
        headerFont.setBold(true);
        header->setFont(headerFont);
        mainLayout->addWidget(header);

        auto *foldersTitle =
            new QLabel(trLocal("Foldery", "Folders"), page);

        QFont sectionFont = foldersTitle->font();
        sectionFont.setPointSize(sectionFont.pointSize() + 1);
        sectionFont.setBold(true);
        foldersTitle->setFont(sectionFont);
        mainLayout->addWidget(foldersTitle);

        auto *foldersGrid = new QGridLayout;
        foldersGrid->setContentsMargins(0, 0, 0, 2);
        foldersGrid->setHorizontalSpacing(10);
        foldersGrid->setVerticalSpacing(7);

        struct FolderDef {
            QStandardPaths::StandardLocation location;
            const char *pl;
            const char *en;
            const char *icon;
        };

        const FolderDef folders[] = {
            {QStandardPaths::DesktopLocation,
             "Pulpit", "Desktop", "folder"},
            {QStandardPaths::DocumentsLocation,
             "Dokumenty", "Documents", "folder-documents"},
            {QStandardPaths::DownloadLocation,
             "Pobrane", "Downloads", "folder-download"},
            {QStandardPaths::PicturesLocation,
             "Obrazy", "Pictures", "folder-pictures"},
            {QStandardPaths::MusicLocation,
             "Muzyka", "Music", "folder-music"},
            {QStandardPaths::MoviesLocation,
             "Filmy", "Videos", "folder-videos"},
        };

        int folderIndex = 0;

        for (const FolderDef &def : folders) {
            const QString path =
                QStandardPaths::writableLocation(def.location);

            if (path.isEmpty()) {
                continue;
            }

            ClickableFrame *card = makeFolderCard(
                trLocal(def.pl, def.en),
                path,
                QString::fromLatin1(def.icon),
                page);

            connect(
                card,
                &ClickableFrame::activated,
                this,
                [this](const QUrl &url) {
                    navigateTo(url, true);
                });

            foldersGrid->addWidget(
                card,
                folderIndex / 3,
                folderIndex % 3);

            ++folderIndex;
        }

        for (int col = 0; col < 3; ++col) {
            foldersGrid->setColumnStretch(col, 1);
        }

        mainLayout->addLayout(foldersGrid);

        auto *drivesTitle = new QLabel(
            trLocal("Urządzenia i dyski", "Devices and drives"),
            page);
        drivesTitle->setFont(sectionFont);
        mainLayout->addWidget(drivesTitle);

        m_homeStatus =
            new QLabel(trLocal("Wczytywanie…", "Loading…"), page);
        m_homeStatus->setForegroundRole(QPalette::PlaceholderText);
        mainLayout->addWidget(m_homeStatus);

        m_drivesGrid = new QGridLayout;
        m_drivesGrid->setContentsMargins(0, 0, 0, 0);
        m_drivesGrid->setHorizontalSpacing(10);
        m_drivesGrid->setVerticalSpacing(7);
        m_drivesGrid->setColumnStretch(0, 1);
        m_drivesGrid->setColumnStretch(1, 1);
        mainLayout->addLayout(m_drivesGrid);

        mainLayout->addStretch(1);

        m_homePage = scroll;
        m_contentStack->addWidget(m_homePage);
    }

    void buildDirectoryPage()
    {
        auto *page = new QWidget(m_contentStack);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 14, 18, 18);
        layout->setSpacing(8);

        m_adminBanner = new QFrame(page);
        m_adminBanner->setObjectName(
            QStringLiteral("adminBanner"));

        auto *adminBannerLayout =
            new QHBoxLayout(m_adminBanner);
        adminBannerLayout->setContentsMargins(
            8, 5, 8, 5);
        adminBannerLayout->setSpacing(8);

        auto *adminIcon =
            new QLabel(m_adminBanner);
        adminIcon->setPixmap(
            themedIcon(
                QStringLiteral("security-high"))
                .pixmap(20, 20));

        auto *adminText =
            new QLabel(
                trLocal(
                    "TRYB ADMINISTRATORA — operacje w tej lokalizacji mogą modyfikować chronione pliki systemowe.",
                    "ADMINISTRATOR MODE — operations in this location may modify protected system files."),
                m_adminBanner);
        adminText->setObjectName(
            QStringLiteral("adminBannerText"));
        adminText->setWordWrap(true);

        auto *leaveAdmin =
            new QPushButton(
                themedIcon(
                    QStringLiteral("system-log-out")),
                trLocal(
                    "Wyjdź z trybu administratora",
                    "Leave administrator mode"),
                m_adminBanner);
        leaveAdmin->setFlat(true);

        connect(
            leaveAdmin,
            &QPushButton::clicked,
            this,
            [this] {
                if (!isAdminUrl(m_currentUrl)) {
                    return;
                }

                navigateTo(
                    ordinaryFileUrlForAdmin(
                        m_currentUrl),
                    true);
            });

        adminBannerLayout->addWidget(adminIcon);
        adminBannerLayout->addWidget(adminText, 1);
        adminBannerLayout->addWidget(leaveAdmin);

        m_adminBanner->hide();
        layout->addWidget(m_adminBanner);

        m_directoryTitle = new QLabel(page);

        QFont titleFont = m_directoryTitle->font();
        titleFont.setPointSize(titleFont.pointSize() + 3);
        titleFont.setBold(true);
        m_directoryTitle->setFont(titleFont);

        layout->addWidget(m_directoryTitle);

        m_directoryStatus = new QLabel(page);
        m_directoryStatus->setForegroundRole(
            QPalette::PlaceholderText);
        layout->addWidget(m_directoryStatus);

        m_searchProgressFrame = new QFrame(page);
        m_searchProgressFrame->setObjectName(
            QStringLiteral("searchProgressFrame"));

        auto *searchProgressLayout =
            new QHBoxLayout(m_searchProgressFrame);
        searchProgressLayout->setContentsMargins(0, 0, 0, 0);
        searchProgressLayout->setSpacing(8);

        m_searchProgressBar =
            new QProgressBar(m_searchProgressFrame);
        m_searchProgressBar->setRange(0, 100);
        m_searchProgressBar->setValue(0);
        m_searchProgressBar->setTextVisible(false);
        m_searchProgressBar->setMaximumHeight(7);

        m_cancelSearchButton =
            new QPushButton(
                themedIcon(QStringLiteral("process-stop")),
                trLocal("Stop", "Stop"),
                m_searchProgressFrame);
        m_cancelSearchButton->setFlat(true);
        m_cancelSearchButton->setToolTip(
            trLocal(
                "Przerwij wyszukiwanie",
                "Stop search"));

        connect(
            m_cancelSearchButton,
            &QPushButton::clicked,
            this,
            [this] {
                cancelActiveSearch(true);
            });

        searchProgressLayout->addWidget(
            m_searchProgressBar,
            1);
        searchProgressLayout->addWidget(
            m_cancelSearchButton);

        m_searchProgressFrame->hide();
        layout->addWidget(m_searchProgressFrame);

        m_searchRenderTimer.setSingleShot(true);
        m_searchRenderTimer.setInterval(120);
        connect(
            &m_searchRenderTimer,
            &QTimer::timeout,
            this,
            [this] {
                if (isSearchLocation(m_currentUrl)) {
                    renderDirectoryItems();
                }
            });

        m_directoryViewStack = new QStackedWidget(page);

        m_directoryList = new DirectoryListWidget(m_directoryViewStack);
        m_directoryList->setObjectName(
            QStringLiteral("directoryList"));
        m_directoryList->setResizeMode(QListView::Adjust);
        m_directoryList->setMovement(QListView::Static);
        m_directoryList->setSelectionMode(
            QAbstractItemView::ExtendedSelection);
        m_directoryList->setContextMenuPolicy(
            Qt::CustomContextMenu);
        m_directoryList->setAlwaysShowFullNames(
            m_alwaysShowFullNames);

        m_directoryDetails =
            new DirectoryTreeWidget(m_directoryViewStack);
        m_directoryDetails->setObjectName(
            QStringLiteral("directoryDetails"));
        m_directoryDetails->setColumnCount(5);
        m_directoryDetails->setHeaderLabels(
            {
                trLocal("Nazwa", "Name"),
                trLocal("Typ", "Type"),
                trLocal("Rozmiar", "Size"),
                trLocal("Zmodyfikowano", "Date modified"),
                trLocal("Lokalizacja", "Location")
            });
        m_directoryDetails->setRootIsDecorated(false);
        m_directoryDetails->setUniformRowHeights(true);
        m_directoryDetails->setAllColumnsShowFocus(true);
        m_directoryDetails->setSelectionMode(
            QAbstractItemView::ExtendedSelection);
        m_directoryDetails->setContextMenuPolicy(
            Qt::CustomContextMenu);
        m_directoryDetails->setIconSize(QSize(22, 22));

        QHeaderView *header =
            m_directoryDetails->header();
        header->setStretchLastSection(false);
        header->setSectionsMovable(true);
        header->setSectionResizeMode(
            0,
            QHeaderView::Stretch);
        header->setSectionResizeMode(
            1,
            QHeaderView::Interactive);
        header->setSectionResizeMode(
            2,
            QHeaderView::ResizeToContents);
        header->setSectionResizeMode(
            3,
            QHeaderView::ResizeToContents);
        header->setSectionResizeMode(
            4,
            QHeaderView::Interactive);
        m_directoryDetails->setColumnWidth(1, 220);
        m_directoryDetails->setColumnWidth(4, 320);
        m_directoryDetails->setColumnHidden(4, true);

        m_directoryViewStack->addWidget(m_directoryList);
        m_directoryViewStack->addWidget(m_directoryDetails);

        layout->addWidget(m_directoryViewStack, 1);

        connect(
            m_directoryList,
            &QListWidget::itemDoubleClicked,
            this,
            [this](QListWidgetItem *item) {
                activateDirectoryItem(item);
            });

        connect(
            m_directoryList,
            &QListWidget::customContextMenuRequested,
            this,
            [this](const QPoint &pos) {
                showDirectoryContextMenu(pos);
            });

        connect(
            m_directoryList,
            &QListWidget::itemSelectionChanged,
            this,
            &ThisPcWindow::updateFileActionStates);

        connect(
            m_directoryList,
            &DirectoryListWidget::urlsDropped,
            this,
            &ThisPcWindow::handleDroppedUrls);

        connect(
            m_directoryDetails,
            &QTreeWidget::itemDoubleClicked,
            this,
            [this](QTreeWidgetItem *item, int) {
                activateDetailsItem(item);
            });

        connect(
            m_directoryDetails,
            &QTreeWidget::customContextMenuRequested,
            this,
            [this](const QPoint &pos) {
                showDirectoryDetailsContextMenu(pos);
            });

        connect(
            m_directoryDetails,
            &QTreeWidget::itemSelectionChanged,
            this,
            &ThisPcWindow::updateFileActionStates);

        connect(
            m_directoryDetails,
            &DirectoryTreeWidget::urlsDropped,
            this,
            &ThisPcWindow::handleDroppedUrls);

        installPaneShortcuts(m_directoryViewStack, PaneId::Primary);
        installPaneShortcuts(m_splitPane->shortcutScope(), PaneId::Split);

        m_directoryPage = page;
        m_contentStack->addWidget(m_directoryPage);

        applyDirectoryViewMode(false);
    }

    void installPaneShortcuts(QWidget *scope, PaneId pane)
    {
        auto addViewShortcut =
            [this, scope, pane](const QKeySequence &sequence,
                   auto callback) {
            auto *shortcut =
                new QShortcut(sequence, scope);
            shortcut->setContext(
                Qt::WidgetWithChildrenShortcut);
            connect(
                shortcut,
                &QShortcut::activated,
                this,
                [this, pane, callback] {
                    setActivePane(pane);
                    callback();
                });
        };

        addViewShortcut(
            QKeySequence::Copy,
            [this] { putSelectionOnClipboard(false); });

        addViewShortcut(
            QKeySequence::Cut,
            [this] { putSelectionOnClipboard(true); });

        addViewShortcut(
            QKeySequence::Paste,
            [this] { pasteClipboard(); });

        addViewShortcut(
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N),
            [this] { createNewFolder(); });

        addViewShortcut(
            QKeySequence(Qt::Key_F2),
            [this] { renameSelected(); });

        addViewShortcut(
            QKeySequence(Qt::Key_Delete),
            [this] { trashSelected(); });

        addViewShortcut(
            QKeySequence::SelectAll,
            [this] { selectAllDirectoryItems(); });

        addViewShortcut(QKeySequence(Qt::ALT | Qt::Key_Return),
                        [this] { showSelectedProperties(); });
    }

    void buildSidebar()
    {
        auto *placesSection =
            new CollapsibleSection(
                trLocal("Miejsca", "Places"),
                QStringLiteral("places"),
                m_sidebar);

        m_sidebarLayout->addWidget(placesSection);

        m_thisPcButton = addSidebarLocation(
            placesSection->contentLayout(),
            trLocal("Ten komputer", "This PC"),
            QStringLiteral("computer"),
            kThisPcUrl);

        const QString home = QDir::homePath();

        addSidebarLocation(
            placesSection->contentLayout(),
            trLocal("Katalog domowy", "Home"),
            QStringLiteral("user-home"),
            QUrl::fromLocalFile(home));

        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::DesktopLocation,
            trLocal("Pulpit", "Desktop"),
            QStringLiteral("folder"));

        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::DocumentsLocation,
            trLocal("Dokumenty", "Documents"),
            QStringLiteral("folder-documents"));

        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::DownloadLocation,
            trLocal("Pobrane", "Downloads"),
            QStringLiteral("folder-download"));

        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::MusicLocation,
            trLocal("Muzyka", "Music"),
            QStringLiteral("folder-music"));

        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::PicturesLocation,
            trLocal("Obrazy", "Pictures"),
            QStringLiteral("folder-pictures"));

        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::MoviesLocation,
            trLocal("Filmy", "Videos"),
            QStringLiteral("folder-videos"));

        const QString gamesPath =
            QDir(home).filePath(QStringLiteral("Games"));

        if (QFileInfo::exists(gamesPath)
            && QFileInfo(gamesPath).isDir()) {
            addSidebarLocation(
                placesSection->contentLayout(),
                QStringLiteral("Games"),
                QStringLiteral("applications-games"),
                QUrl::fromLocalFile(gamesPath));
        }

        addSidebarLocation(
            placesSection->contentLayout(),
            trLocal("Kosz", "Trash"),
            QStringLiteral("user-trash"),
            QUrl(QStringLiteral("trash:/")));

        auto *remoteSection =
            new CollapsibleSection(
                trLocal("Zdalne", "Remote"),
                QStringLiteral("remote"),
                m_sidebar);

        m_sidebarLayout->addWidget(remoteSection);

        addSidebarLocation(
            remoteSection->contentLayout(),
            trLocal("Sieć", "Network"),
            QStringLiteral("network-workgroup"),
            QUrl(QStringLiteral("remote:/")));

        auto *devicesSection =
            new CollapsibleSection(
                trLocal("Urządzenia", "Devices"),
                QStringLiteral("devices"),
                m_sidebar);

        m_sidebarLayout->addWidget(devicesSection);
        m_devicesLayout = devicesSection->contentLayout();

        m_sidebarLayout->addStretch(1);
    }

    SidebarButton *addSidebarLocation(
        QVBoxLayout *layout,
        const QString &name,
        const QString &iconName,
        const QUrl &url)
    {
        auto *button =
            new SidebarButton(name, iconName, url, m_sidebar);

        connect(
            button,
            &SidebarButton::activated,
            this,
            [this](const QUrl &target) {
                navigateTo(target, true);
            });

        connect(
            button,
            &SidebarButton::openInNewTabRequested,
            this,
            [this](const QUrl &target, bool makeCurrent) {
                createNewTab(target, makeCurrent);
            });

        connect(
            button,
            &SidebarButton::openInNewWindowRequested,
            this,
            &ThisPcWindow::openInNewWindow);
        connect(
            button,
            &SidebarButton::openInSplitPaneRequested,
            this,
            &ThisPcWindow::openInSplitPane);

        layout->addWidget(button);
        m_staticSidebarButtons.push_back(button);
        return button;
    }

    void addStandardSidebarLocation(
        QVBoxLayout *layout,
        QStandardPaths::StandardLocation location,
        const QString &name,
        const QString &iconName)
    {
        const QString path =
            QStandardPaths::writableLocation(location);

        if (!path.isEmpty()) {
            addSidebarLocation(
                layout,
                name,
                iconName,
                QUrl::fromLocalFile(path));
        }
    }

    QIcon searchScopeMenuIcon() const
    {
        const QSize size(24, 20);
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        themedIcon(QStringLiteral("edit-find")).paint(
            &painter,
            QRect(0, 1, 17, 17),
            Qt::AlignCenter,
            QIcon::Normal);

        const QIcon arrow = style()->standardIcon(QStyle::SP_ArrowDown);
        arrow.paint(
            &painter,
            QRect(15, 9, 8, 8),
            Qt::AlignCenter,
            QIcon::Normal);

        return QIcon(pixmap);
    }

    void updateSearchControls()
    {
        auto checkGroup =
            [](QActionGroup *group, int value) {
            if (!group) {
                return;
            }

            for (QAction *action : group->actions()) {
                action->setChecked(
                    action->data().toInt() == value);
            }
        };

        checkGroup(
            m_searchScopeGroup,
            m_searchScopeMode);
        checkGroup(
            m_searchTypeGroup,
            m_searchTypeFilter);
        checkGroup(
            m_searchDateGroup,
            m_searchDateFilter);
        checkGroup(
            m_searchSizeGroup,
            m_searchSizeFilter);

        if (m_searchScopeAction) {
            QString scopeName;

            switch (m_searchScopeMode) {
            case 1:
                scopeName = trLocal(
                    "Bieżący dysk",
                    "Current drive");
                break;
            case 2:
                scopeName = trLocal(
                    "Ten komputer",
                    "This PC");
                break;
            case 0:
            default:
                scopeName = trLocal(
                    "Bieżący folder",
                    "Current folder");
                break;
            }

            m_searchScopeAction->setIcon(
                searchScopeMenuIcon());
            m_searchScopeAction->setToolTip(
                isPolish()
                    ? QStringLiteral(
                        "Zakres wyszukiwania: %1\nKliknij lupę ze strzałką, aby zmienić zakres.")
                        .arg(scopeName)
                    : QStringLiteral(
                        "Search scope: %1\nClick the magnifier arrow to change scope.")
                        .arg(scopeName));
            m_searchScopeAction->setEnabled(
                !isSearchLocation(m_currentUrl));
        }

        if (m_searchFilterButton) {
            int activeFilters = 0;
            activeFilters +=
                m_searchTypeFilter != 0 ? 1 : 0;
            activeFilters +=
                m_searchDateFilter != 0 ? 1 : 0;
            activeFilters +=
                m_searchSizeFilter != 0 ? 1 : 0;

            m_searchFilterButton->setToolTip(
                activeFilters == 0
                    ? trLocal(
                        "Filtry wyszukiwania",
                        "Search filters")
                    : (isPolish()
                        ? QStringLiteral(
                            "Filtry wyszukiwania (%1 aktywne)")
                            .arg(activeFilters)
                        : QStringLiteral(
                            "Search filters (%1 active)")
                            .arg(activeFilters)));
        }
    }

    QUrl bestDriveRootForUrl(const QUrl &url) const
    {
        QUrl best;
        int bestDepth = -1;

        for (const DriveInfo &drive :
             std::as_const(m_drives)) {
            if (!drive.targetUrl.isValid()
                || !isWithinLocation(
                    url,
                    drive.targetUrl)) {
                continue;
            }

            const int depth =
                locationDepth(drive.targetUrl);

            if (depth > bestDepth) {
                bestDepth = depth;
                best = drive.targetUrl;
            }
        }

        return best;
    }

    QList<QUrl> wholeComputerSearchRoots() const
    {
        QList<QUrl> roots;
        QSet<QString> seen;

        for (const DriveInfo &drive :
             std::as_const(m_drives)) {
            if (!drive.targetUrl.isValid()) {
                continue;
            }

            const QUrl root =
                normalizedUrl(drive.targetUrl);

            const QString key =
                root.toString(QUrl::FullyEncoded);

            if (seen.contains(key)) {
                continue;
            }

            seen.insert(key);
            roots.push_back(root);
        }

        if (roots.isEmpty()) {
            roots.push_back(
                QUrl::fromLocalFile(
                    QStringLiteral("/")));
        }

        return roots;
    }

    QUrl searchContextUrl() const
    {
        if (isSearchLocation(m_currentUrl)) {
            const QUrl base =
                searchBaseFromUrl(m_currentUrl);

            if (base.isValid()) {
                return base;
            }

            return QUrl::fromLocalFile(
                QDir::homePath());
        }

        if (sameLocation(
                m_currentUrl,
                kThisPcUrl)) {
            return QUrl::fromLocalFile(
                QDir::homePath());
        }

        return m_currentUrl;
    }

    void startSearchFromUi()
    {
        if (!m_searchEdit) {
            return;
        }

        const QString query =
            m_searchEdit->text().trimmed();

        if (query.isEmpty()) {
            return;
        }

        int scope = m_searchScopeMode;
        QUrl base;

        if (sameLocation(
                m_currentUrl,
                kThisPcUrl)) {
            scope = 2;
        }

        const QUrl context =
            searchContextUrl();

        if (scope == 0) {
            base = context;
        } else if (scope == 1) {
            base =
                bestDriveRootForUrl(context);

            if (!base.isValid()) {
                base = context;
            }
        }

        navigateTo(
            makeSearchLocation(
                query,
                scope,
                base,
                m_searchTypeFilter,
                m_searchDateFilter,
                m_searchSizeFilter),
            true);
    }

    void syncCurrentSearchFilters()
    {
        if (!isSearchLocation(m_currentUrl)) {
            return;
        }

        const QUrl updated =
            makeSearchLocation(
                searchQueryFromUrl(m_currentUrl),
                searchIntParameter(
                    m_currentUrl,
                    QStringLiteral("scope"),
                    m_searchScopeMode),
                searchBaseFromUrl(m_currentUrl),
                m_searchTypeFilter,
                m_searchDateFilter,
                m_searchSizeFilter);

        m_currentUrl = updated;

        if (m_historyIndex >= 0
            && m_historyIndex < m_history.size()) {
            m_history[m_historyIndex] = updated;
        }

        syncActiveTabState();
        updateActiveTabPresentation();
    }

    bool fileMatchesAdvancedSearch(
        const FileInfo &file,
        QMimeDatabase &mimeDatabase) const
    {
        const QMimeType mime =
            resolvedMimeType(
                file,
                mimeDatabase);

        if (m_searchTypeFilter != 0) {
            const QString mimeName =
                mime.isValid()
                    ? mime.name()
                    : file.mimeType;

            bool typeMatches = false;

            switch (m_searchTypeFilter) {
            case 1:
                typeMatches = file.isDir;
                break;
            case 2:
                typeMatches =
                    !file.isDir
                    && mimeName.startsWith(
                        QStringLiteral("image/"));
                break;
            case 3:
                typeMatches =
                    !file.isDir
                    && (mimeName.startsWith(
                            QStringLiteral("text/"))
                        || mimeName
                            == QStringLiteral("application/pdf")
                        || mimeName.contains(
                            QStringLiteral("officedocument"))
                        || mimeName.contains(
                            QStringLiteral("opendocument"))
                        || mimeName.contains(
                            QStringLiteral("msword"))
                        || mimeName.contains(
                            QStringLiteral("rtf")));
                break;
            case 4:
                typeMatches =
                    !file.isDir
                    && mimeName.startsWith(
                        QStringLiteral("audio/"));
                break;
            case 5:
                typeMatches =
                    !file.isDir
                    && mimeName.startsWith(
                        QStringLiteral("video/"));
                break;
            case 6:
                typeMatches =
                    !file.isDir
                    && (mimeName.contains(
                            QStringLiteral("zip"))
                        || mimeName.contains(
                            QStringLiteral("archive"))
                        || mimeName.contains(
                            QStringLiteral("compressed"))
                        || mimeName.contains(
                            QStringLiteral("rar"))
                        || mimeName.contains(
                            QStringLiteral("7z"))
                        || mimeName.contains(
                            QStringLiteral("tar")));
                break;
            default:
                typeMatches = true;
                break;
            }

            if (!typeMatches) {
                return false;
            }
        }

        if (m_searchDateFilter != 0) {
            if (file.modificationTime <= 0) {
                return false;
            }

            qint64 days = 0;

            switch (m_searchDateFilter) {
            case 1:
                days = 1;
                break;
            case 2:
                days = 7;
                break;
            case 3:
                days = 30;
                break;
            case 4:
                days = 365;
                break;
            default:
                break;
            }

            const qint64 cutoff =
                QDateTime::currentSecsSinceEpoch()
                - days * 24 * 60 * 60;

            if (file.modificationTime < cutoff) {
                return false;
            }
        }

        if (m_searchSizeFilter != 0) {
            if (file.isDir || file.size < 0) {
                return false;
            }

            const qint64 mib =
                1024LL * 1024LL;
            const qint64 gib =
                1024LL * mib;

            bool sizeMatches = false;

            switch (m_searchSizeFilter) {
            case 1:
                sizeMatches =
                    file.size < mib;
                break;
            case 2:
                sizeMatches =
                    file.size >= mib
                    && file.size < 100LL * mib;
                break;
            case 3:
                sizeMatches =
                    file.size >= 100LL * mib
                    && file.size < gib;
                break;
            case 4:
                sizeMatches =
                    file.size >= gib;
                break;
            default:
                sizeMatches = true;
                break;
            }

            if (!sizeMatches) {
                return false;
            }
        }

        return true;
    }

    void updateSearchProgress()
    {
        if (!m_searchProgressBar
            || m_searchTotalRoots <= 0) {
            return;
        }

        unsigned long total = 0;

        for (auto it =
                 m_searchProgressValues.constBegin();
             it != m_searchProgressValues.constEnd();
             ++it) {
            total +=
                static_cast<unsigned long>(
                    std::clamp(
                        it.value(),
                        0,
                        100));
        }

        const int progress =
            static_cast<int>(
                total
                / static_cast<unsigned long>(
                    m_searchTotalRoots));

        m_searchProgressBar->setValue(
            std::clamp(progress, 0, 100));
    }

    void updateSearchStatusLabel()
    {
        if (!m_directoryStatus
            || !isSearchLocation(m_currentUrl)) {
            return;
        }

        const QString scopeLabel =
            m_searchScopeMode == 2
                ? trLocal(
                    "Ten komputer",
                    "This PC")
                : (m_searchScopeMode == 1
                    ? trLocal(
                        "Bieżący dysk",
                        "Current drive")
                    : trLocal(
                        "Bieżący folder",
                        "Current folder"));

        if (m_searchInProgress) {
            m_directoryStatus->setText(
                isPolish()
                    ? QStringLiteral(
                        "%1 wyników • %2/%3 lokalizacji • %4")
                        .arg(m_searchVisibleCount)
                        .arg(m_searchCompletedRoots)
                        .arg(m_searchTotalRoots)
                        .arg(scopeLabel)
                    : QStringLiteral(
                        "%1 results • %2/%3 locations • %4")
                        .arg(m_searchVisibleCount)
                        .arg(m_searchCompletedRoots)
                        .arg(m_searchTotalRoots)
                        .arg(scopeLabel));
        } else {
            QString message =
                isPolish()
                    ? QStringLiteral(
                        "%1 wyników • zakres: %2")
                        .arg(m_searchVisibleCount)
                        .arg(scopeLabel)
                    : QStringLiteral(
                        "%1 results • scope: %2")
                        .arg(m_searchVisibleCount)
                        .arg(scopeLabel);

            if (m_searchErrors > 0) {
                message +=
                    isPolish()
                        ? QStringLiteral(
                            " • %1 lokalizacji z błędem")
                            .arg(m_searchErrors)
                        : QStringLiteral(
                            " • %1 locations with errors")
                            .arg(m_searchErrors);
            }

            m_directoryStatus->setText(message);
        }
    }

    void cancelActiveSearch(bool userRequested)
    {
        if (!m_searchInProgress
            && m_searchJobs.isEmpty()) {
            return;
        }

        ++m_searchGeneration;

        for (const QPointer<KIO::ListJob> &job :
             std::as_const(m_searchJobs)) {
            if (job) {
                job->kill(KJob::Quietly);
            }
        }

        m_searchJobs.clear();
        m_searchProgressValues.clear();
        m_searchInProgress = false;

        if (m_searchRenderTimer.isActive()) {
            m_searchRenderTimer.stop();
        }

        if (m_searchProgressFrame) {
            m_searchProgressFrame->hide();
        }

        if (m_stopSearchAction) {
            m_stopSearchAction->setVisible(false);
        }

        if (userRequested) {
            renderDirectoryItems();

            statusBar()->showMessage(
                trLocal(
                    "Wyszukiwanie anulowane",
                    "Search canceled"),
                4000);
        }
    }

    void scheduleSearchRender()
    {
        if (!m_searchRenderTimer.isActive()) {
            m_searchRenderTimer.start();
        }
    }

    void loadSearchLocation(const QUrl &url)
    {
        cancelActiveSearch(false);

        m_pendingFiles.clear();
        m_searchSeenUrls.clear();
        m_directoryList->clear();
        m_directoryDetails->clear();
        m_directoryList->setDropDirectory(QUrl());
        m_directoryDetails->setDropDirectory(QUrl());

        const QString query =
            searchQueryFromUrl(url);

        m_searchScopeMode =
            std::clamp(
                searchIntParameter(
                    url,
                    QStringLiteral("scope"),
                    2),
                0,
                2);

        m_searchTypeFilter =
            std::clamp(
                searchIntParameter(
                    url,
                    QStringLiteral("type"),
                    0),
                0,
                6);

        m_searchDateFilter =
            std::clamp(
                searchIntParameter(
                    url,
                    QStringLiteral("date"),
                    0),
                0,
                4);

        m_searchSizeFilter =
            std::clamp(
                searchIntParameter(
                    url,
                    QStringLiteral("size"),
                    0),
                0,
                4);

        updateSearchControls();

        m_directoryDetails->setColumnHidden(
            4,
            false);

        m_directoryTitle->setText(
            isPolish()
                ? QStringLiteral("Wyniki wyszukiwania: %1")
                    .arg(query)
                : QStringLiteral("Search results: %1")
                    .arg(query));

        QList<QUrl> roots;

        if (m_searchScopeMode == 2) {
            roots =
                wholeComputerSearchRoots();
        } else {
            QUrl base =
                searchBaseFromUrl(url);

            if (!base.isValid()) {
                base =
                    QUrl::fromLocalFile(
                        QDir::homePath());
            }

            roots.push_back(base);
        }

        m_searchTotalRoots =
            roots.size();
        m_searchCompletedRoots = 0;
        m_searchErrors = 0;
        m_searchVisibleCount = 0;
        m_searchInProgress = true;
        m_searchJobs.clear();
        m_searchProgressValues.clear();

        if (m_searchProgressBar) {
            m_searchProgressBar->setRange(0, 100);
            m_searchProgressBar->setValue(0);
        }

        if (m_searchProgressFrame) {
            m_searchProgressFrame->show();
        }

        if (m_stopSearchAction) {
            m_stopSearchAction->setVisible(true);
        }

        const quint64 generation =
            ++m_searchGeneration;

        updateSearchStatusLabel();

        for (const QUrl &root : std::as_const(roots)) {
            KIO::ListJob *job =
                KIO::listDir(
                    filenameSearchUrl(
                        query,
                        root),
                    KIO::HideProgressInfo);

            job->setUiDelegate(nullptr);
            m_searchJobs.push_back(job);
            m_searchProgressValues.insert(
                job,
                0);

            connect(
                job,
                &KJob::percentChanged,
                this,
                [this, generation, job](
                    KJob *,
                    unsigned long percent) {
                    if (generation
                        != m_searchGeneration) {
                        return;
                    }

                    m_searchProgressValues[job] =
                        static_cast<int>(percent);

                    updateSearchProgress();
                });

            connect(
                job,
                &KIO::ListJob::entries,
                this,
                [this, generation, root](
                    KIO::Job *,
                    const KIO::UDSEntryList &entries) {
                    if (generation
                        != m_searchGeneration) {
                        return;
                    }

                    for (const KIO::UDSEntry &entry :
                         entries) {
                        QString name =
                            entry.stringValue(
                                KIO::UDSEntry::UDS_DISPLAY_NAME);

                        if (name.isEmpty()) {
                            name =
                                entry.stringValue(
                                    KIO::UDSEntry::UDS_NAME);
                        }

                        const QString rawName =
                            entry.stringValue(
                                KIO::UDSEntry::UDS_NAME);

                        if (name.isEmpty()
                            || rawName == QStringLiteral(".")
                            || rawName == QStringLiteral("..")
                            || (!m_showHiddenFiles
                                && rawName.startsWith(
                                    QLatin1Char('.')))) {
                            continue;
                        }

                        FileInfo file;
                        file.name = name;
                        file.mimeType =
                            entry.stringValue(
                                KIO::UDSEntry::UDS_MIME_TYPE);
                        file.iconName =
                            entry.stringValue(
                                KIO::UDSEntry::UDS_ICON_NAME);
                        file.url =
                            childUrlForEntry(
                                root,
                                entry);
                        file.isDir =
                            entryIsDirectory(entry);
                        file.size =
                            entry.numberValue(
                                KIO::UDSEntry::UDS_SIZE,
                                -1);
                        file.modificationTime =
                            entry.numberValue(
                                KIO::UDSEntry::UDS_MODIFICATION_TIME,
                                0);

                        if (!file.url.isValid()
                            || file.url.isEmpty()) {
                            continue;
                        }

                        const QString key =
                            normalizedUrl(file.url)
                                .toString(
                                    QUrl::FullyEncoded);

                        if (m_searchSeenUrls.contains(key)) {
                            continue;
                        }

                        m_searchSeenUrls.insert(key);
                        m_pendingFiles.push_back(file);
                    }

                    scheduleSearchRender();
                });

            connect(
                job,
                &KJob::result,
                this,
                [this, generation, job](KJob *) {
                    if (generation
                        != m_searchGeneration) {
                        return;
                    }

                    m_searchProgressValues[job] = 100;
                    ++m_searchCompletedRoots;

                    if (job->error()) {
                        ++m_searchErrors;
                    }

                    updateSearchProgress();

                    if (m_searchCompletedRoots
                        >= m_searchTotalRoots) {
                        m_searchInProgress = false;

                        if (m_searchRenderTimer.isActive()) {
                            m_searchRenderTimer.stop();
                        }

                        renderDirectoryItems();

                        if (m_searchProgressFrame) {
                            m_searchProgressFrame->hide();
                        }

                        if (m_stopSearchAction) {
                            m_stopSearchAction->setVisible(false);
                        }

                        statusBar()->showMessage(
                            trLocal(
                                "Wyszukiwanie zakończone",
                                "Search completed"),
                            4000);
                    } else {
                        scheduleSearchRender();
                        updateSearchStatusLabel();
                    }
                });
        }
    }

    void setSplitViewEnabled(bool enabled)
    {
        if (!m_splitPane || !m_contentSplitter) {
            return;
        }

        if (m_splitViewAction
            && m_splitViewAction->isChecked() != enabled) {
            QSignalBlocker blocker(m_splitViewAction);
            m_splitViewAction->setChecked(enabled);
        }

        if (enabled) {
            QUrl target = m_currentUrl;

            if (m_activeTab >= 0
                && m_activeTab < m_tabs.size()
                && m_tabs.at(m_activeTab).splitUrl.isValid()) {
                target = m_tabs.at(m_activeTab).splitUrl;
            }

            if (isSearchLocation(target)) {
                const QUrl base = searchBaseFromUrl(target);
                target = base.isValid() ? base : kThisPcUrl;
            }
            if (!target.isValid()) {
                target = kThisPcUrl;
            }

            m_splitPane->show();

            int splitViewMode =
                m_directoryViewMode;
            int splitSortKey =
                m_sortKey;
            bool splitSortAscending =
                m_sortAscending;

            if (m_activeTab >= 0
                && m_activeTab < m_tabs.size()) {
                const TabState &state =
                    m_tabs.at(m_activeTab);

                if (state.splitViewMode >= 0) {
                    splitViewMode =
                        state.splitViewMode;
                    splitSortKey =
                        state.splitSortKey;
                    splitSortAscending =
                        state.splitSortAscending;
                }
            }

            m_splitPane->setViewMode(splitViewMode);
            m_splitPane->setSortState(
                splitSortKey,
                splitSortAscending);
            m_splitPane->setCurrentUrl(target, true);

            QList<int> sizes = m_contentSplitter->sizes();
            if (sizes.size() == 2 && sizes.at(1) < 80) {
                const int total = qMax(700, sizes.at(0) + sizes.at(1));
                m_contentSplitter->setSizes({total / 2, total / 2});
            }
        } else {
            m_splitPane->hide();
            setActivePane(PaneId::Primary);
        }

        if (!m_tabRestoreInProgress) {
            syncActiveTabState();
        }
    }

    void openInSplitPane(const QUrl &rawUrl)
    {
        const QUrl url = normalizedUrl(
            rawUrl.isValid() ? rawUrl : m_currentUrl);

        setSplitViewEnabled(true);
        m_splitPane->setCurrentUrl(url, true);
        syncActiveTabState();
    }

    void swapSplitPanes()
    {
        if (!m_splitPane || !m_splitPane->isVisible()) {
            return;
        }

        const QUrl primary = m_currentUrl;
        const QUrl secondary = m_splitPane->currentUrl();

        if (secondary.isValid()) {
            navigateTo(secondary, true);
        }
        if (primary.isValid()) {
            m_splitPane->setCurrentUrl(primary, true);
        }
        syncActiveTabState();
    }

    void focusPrimaryPane()
    {
        if (m_contentStack->currentWidget() == m_homePage) {
            m_homePage->setFocus(Qt::ShortcutFocusReason);
            return;
        }

        if (m_directoryViewMode == 2 && m_directoryDetails) {
            m_directoryDetails->setFocus(Qt::ShortcutFocusReason);
        } else if (m_directoryList) {
            m_directoryList->setFocus(Qt::ShortcutFocusReason);
        }
    }

    void buildSplitShortcuts()
    {
        auto *focusOther = new QAction(this);
        focusOther->setShortcut(QKeySequence(Qt::Key_F6));
        focusOther->setShortcutContext(Qt::WindowShortcut);
        addAction(focusOther);

        connect(
            focusOther,
            &QAction::triggered,
            this,
            [this] {
                if (!m_splitPane || !m_splitPane->isVisible()) {
                    return;
                }

                if (m_splitPane->viewHasFocus()) {
                    focusPrimaryPane();
                } else {
                    m_splitPane->focusView();
                }
            });
    }

    QIcon tabIconForUrl(const QUrl &url) const
    {
        if (sameLocation(url, kThisPcUrl)) {
            return themedIcon(QStringLiteral("computer"));
        }

        if (isSearchLocation(url)) {
            return themedIcon(QStringLiteral("system-search"));
        }

        if (isAdminUrl(url)) {
            return themedIcon(QStringLiteral("security-high"));
        }

        if (url.scheme() == QStringLiteral("trash")) {
            return themedIcon(QStringLiteral("user-trash"));
        }

        if (url.scheme() == QStringLiteral("remote")) {
            return themedIcon(QStringLiteral("network-workgroup"));
        }

        return themedIcon(QStringLiteral("folder"));
    }

    QString tabTitleForUrl(const QUrl &url) const
    {
        QString title = displayNameForLocation(url);

        if (title.isEmpty()) {
            title = trLocal("Nowa karta", "New tab");
        }

        return title;
    }

    void syncActiveTabState()
    {
        if (m_activeTab < 0
            || m_activeTab >= m_tabs.size()) {
            return;
        }

        if (m_tabRestoreInProgress) {
            return;
        }

        TabState &state = m_tabs[m_activeTab];
        state.currentUrl = m_currentUrl;
        state.history = m_history;
        state.historyIndex = m_historyIndex;
        state.splitEnabled =
            m_splitPane
            && m_splitPane->isVisible();

        if (m_splitPane) {
            if (m_splitPane->currentUrl().isValid()) {
                state.splitUrl =
                    m_splitPane->currentUrl();
            }

            state.splitViewMode =
                m_splitPane->viewMode();
            state.splitSortKey =
                m_splitPane->sortKey();
            state.splitSortAscending =
                m_splitPane->sortAscending();
        }
    }

    void saveSessionState()
    {
        syncActiveTabState();

        QSettings settings;
        settings.setValue(
            QStringLiteral("session/activeTab"),
            m_activeTab);
        settings.setValue(
            QStringLiteral("session/activePane"),
            m_activePane == PaneId::Split ? 1 : 0);

        settings.beginWriteArray(
            QStringLiteral("session/tabs"),
            static_cast<int>(m_tabs.size()));
        for (int i = 0; i < m_tabs.size(); ++i) {
            settings.setArrayIndex(i);
            const TabState &state = m_tabs.at(i);

            settings.setValue(
                QStringLiteral("currentUrl"),
                state.currentUrl.toString(
                    QUrl::FullyEncoded));

            QStringList history;
            history.reserve(state.history.size());
            for (const QUrl &url : state.history) {
                history.push_back(
                    url.toString(QUrl::FullyEncoded));
            }
            settings.setValue(
                QStringLiteral("history"),
                history);
            settings.setValue(
                QStringLiteral("historyIndex"),
                state.historyIndex);
            settings.setValue(
                QStringLiteral("splitEnabled"),
                state.splitEnabled);
            settings.setValue(
                QStringLiteral("splitUrl"),
                state.splitUrl.toString(
                    QUrl::FullyEncoded));
            settings.setValue(
                QStringLiteral("splitViewMode"),
                state.splitViewMode);
            settings.setValue(
                QStringLiteral("splitSortKey"),
                state.splitSortKey);
            settings.setValue(
                QStringLiteral("splitSortAscending"),
                state.splitSortAscending);
        }
        settings.endArray();
        settings.sync();
    }

    bool restoreSessionState()
    {
        if (!m_tabBar || !m_tabs.isEmpty()) {
            return false;
        }

        QSettings settings;
        const int count = settings.beginReadArray(
            QStringLiteral("session/tabs"));
        QList<TabState> restoredTabs;
        restoredTabs.reserve(count);

        for (int i = 0; i < count; ++i) {
            settings.setArrayIndex(i);

            TabState state;
            state.currentUrl = normalizedUrl(
                QUrl(settings.value(
                    QStringLiteral("currentUrl")).toString()));
            if (!state.currentUrl.isValid()) {
                continue;
            }

            const QStringList historyValues =
                settings.value(
                    QStringLiteral("history")).toStringList();
            for (const QString &value : historyValues) {
                const QUrl url = normalizedUrl(QUrl(value));
                if (url.isValid()) {
                    state.history.push_back(url);
                }
            }
            if (state.history.isEmpty()) {
                state.history = {state.currentUrl};
            }
            state.historyIndex = std::clamp(
                settings.value(
                    QStringLiteral("historyIndex"),
                    static_cast<int>(state.history.size()) - 1).toInt(),
                0,
                static_cast<int>(state.history.size()) - 1);

            state.splitEnabled = settings.value(
                QStringLiteral("splitEnabled"),
                false).toBool();
            state.splitUrl = normalizedUrl(
                QUrl(settings.value(
                    QStringLiteral("splitUrl")).toString()));
            if (!state.splitUrl.isValid()) {
                state.splitUrl = state.currentUrl;
            }
            state.splitViewMode = settings.value(
                QStringLiteral("splitViewMode"),
                -1).toInt();
            state.splitSortKey = std::clamp(
                settings.value(
                    QStringLiteral("splitSortKey"),
                    m_sortKey).toInt(),
                0,
                3);
            state.splitSortAscending = settings.value(
                QStringLiteral("splitSortAscending"),
                m_sortAscending).toBool();

            restoredTabs.push_back(state);
        }
        settings.endArray();

        if (restoredTabs.isEmpty()) {
            return false;
        }

        m_tabs = restoredTabs;
        m_tabChangeInProgress = true;
        for (int i = 0; i < m_tabs.size(); ++i) {
            const TabState &state = m_tabs.at(i);
            m_tabBar->addTab(
                tabIconForUrl(state.currentUrl),
                tabTitleForUrl(state.currentUrl));
            m_tabBar->setTabToolTip(
                i,
                urlForDisplay(state.currentUrl));
        }

        const int activeIndex = std::clamp(
            settings.value(
                QStringLiteral("session/activeTab"),
                0).toInt(),
            0,
            static_cast<int>(m_tabs.size()) - 1);
        m_tabBar->setCurrentIndex(activeIndex);
        m_tabChangeInProgress = false;

        m_activeTab = -1;
        switchToTab(activeIndex);

        const bool restoreSplitFocus =
            settings.value(
                QStringLiteral("session/activePane"),
                0).toInt() == 1;
        setActivePane(
            restoreSplitFocus
                && m_splitPane
                && m_splitPane->isVisible()
                ? PaneId::Split
                : PaneId::Primary);

        refreshRestoredSessionGeometry();
        return true;
    }

    void refreshRestoredSessionGeometry()
    {
        // Session restore happens before the window has completed its first
        // layout pass.  Re-run the tab-strip/splitter geometry after the event
        // loop starts so the active-pane top border is aligned with the final
        // tab-strip height immediately, rather than only after switching tabs.
        QTimer::singleShot(0, this, [this] {
            if (m_tabBar) {
                m_tabBar->updateGeometry();
                m_tabBar->update();
            }
            if (m_tabStrip) {
                if (m_tabStrip->layout()) {
                    m_tabStrip->layout()->invalidate();
                    m_tabStrip->layout()->activate();
                }
                m_tabStrip->updateGeometry();
                m_tabStrip->update();
            }
            if (m_contentSplitter) {
                m_contentSplitter->updateGeometry();
                m_contentSplitter->update();
            }
            if (m_primaryPane) {
                m_primaryPane->updateGeometry();
                m_primaryPane->update();
            }
            if (m_splitPane && m_splitPane->isVisible()) {
                m_splitPane->updateGeometry();
                m_splitPane->update();
            }
            if (centralWidget() && centralWidget()->layout()) {
                centralWidget()->layout()->invalidate();
                centralWidget()->layout()->activate();
            }
        });
    }

    void updateActiveTabPresentation()
    {
        if (!m_tabBar
            || m_activeTab < 0
            || m_activeTab >= m_tabs.size()) {
            return;
        }

        const QUrl url = m_currentUrl;
        const QString title = tabTitleForUrl(url);

        m_tabBar->setTabText(m_activeTab, title);
        m_tabBar->setTabIcon(m_activeTab, tabIconForUrl(url));
        m_tabBar->setTabToolTip(
            m_activeTab,
            urlForDisplay(url));

        setWindowTitle(
            isPolish()
                ? QStringLiteral("%1 — Ten komputer").arg(title)
                : QStringLiteral("%1 — This PC").arg(title));
    }

    void openInNewWindow(const QUrl &rawUrl)
    {
        const QUrl url = normalizedUrl(
            rawUrl.isValid() ? rawUrl : kThisPcUrl);

        const QString executable =
            QCoreApplication::applicationFilePath();

        if (executable.isEmpty()
            || !QProcess::startDetached(
                executable,
                {url.toString(QUrl::FullyEncoded)})) {
            QMessageBox::warning(
                this,
                trLocal(
                    "Nowe okno",
                    "New window"),
                trLocal(
                    "Nie udało się otworzyć nowego okna.",
                    "Could not open a new window."));
        }
    }

    void createNewTab(
        const QUrl &rawUrl = kThisPcUrl,
        bool makeCurrent = true)
    {
        const QUrl url = normalizedUrl(
            rawUrl.isValid() ? rawUrl : kThisPcUrl);

        TabState state;
        state.currentUrl = url;
        state.history = {url};
        state.historyIndex = 0;
        state.splitEnabled = false;
        state.splitUrl = url;
        state.splitViewMode = -1;
        state.splitSortKey = m_sortKey;
        state.splitSortAscending =
            m_sortAscending;

        const int index = m_tabs.size();
        m_tabs.push_back(state);

        m_tabChangeInProgress = true;
        m_tabBar->addTab(
            tabIconForUrl(url),
            tabTitleForUrl(url));
        m_tabBar->setTabToolTip(
            index,
            urlForDisplay(url));

        if (makeCurrent) {
            m_tabBar->setCurrentIndex(index);
        }
        m_tabChangeInProgress = false;

        if (makeCurrent) {
            switchToTab(index);
        }
    }

    void duplicateTab(int index)
    {
        if (index < 0 || index >= m_tabs.size()) {
            return;
        }

        if (index == m_activeTab) {
            syncActiveTabState();
        }

        const TabState source = m_tabs.at(index);
        const int newIndex = m_tabs.size();
        m_tabs.push_back(source);

        m_tabChangeInProgress = true;
        m_tabBar->addTab(
            tabIconForUrl(source.currentUrl),
            tabTitleForUrl(source.currentUrl));
        m_tabBar->setTabToolTip(
            newIndex,
            urlForDisplay(source.currentUrl));
        m_tabBar->setCurrentIndex(newIndex);
        m_tabChangeInProgress = false;

        switchToTab(newIndex);
    }

    void reopenClosedTab()
    {
        if (m_closedTabs.isEmpty()) {
            return;
        }

        const TabState state = m_closedTabs.takeLast();
        const int index = m_tabs.size();
        m_tabs.push_back(state);

        m_tabChangeInProgress = true;
        m_tabBar->addTab(
            tabIconForUrl(state.currentUrl),
            tabTitleForUrl(state.currentUrl));
        m_tabBar->setTabToolTip(
            index,
            urlForDisplay(state.currentUrl));
        m_tabBar->setCurrentIndex(index);
        m_tabChangeInProgress = false;

        switchToTab(index);
    }

    QUrl tabDropDirectory(int index) const
    {
        if (index < 0 || index >= m_tabs.size()) return {};
        const QUrl url = index == m_activeTab ? m_currentUrl : m_tabs.at(index).currentUrl;
        const QString scheme = url.scheme().toLower();
        // The existing copy/move handler is for directories, not virtual roots
        // or Trash (which requires KIO::trash rather than KIO::move).
        if (!url.isValid() || scheme.isEmpty()
            || scheme == QStringLiteral("thispc")
            || scheme == QStringLiteral("trash")
            || scheme == QStringLiteral("remote")
            || scheme == QStringLiteral("filenamesearch")
            || isSearchLocation(url)
            || !KProtocolManager::supportsListing(url)
            || !KProtocolManager::supportsWriting(url)) {
            return {};
        }
        return url;
    }

    void switchToTab(int index)
    {
        if (index < 0 || index >= m_tabs.size()) {
            return;
        }

        if (index == m_activeTab) {
            updateActiveTabPresentation();
            return;
        }

        syncActiveTabState();

        m_activeTab = index;
        const TabState state = m_tabs.at(index);
        m_history = state.history;
        m_historyIndex = state.historyIndex;

        if (m_history.isEmpty()) {
            m_history = {state.currentUrl};
            m_historyIndex = 0;
        }

        if (m_historyIndex < 0
            || m_historyIndex >= m_history.size()) {
            m_historyIndex = m_history.size() - 1;
        }

        m_tabChangeInProgress = true;
        if (m_tabBar->currentIndex() != index) {
            m_tabBar->setCurrentIndex(index);
        }
        m_tabChangeInProgress = false;

        m_tabRestoreInProgress = true;
        if (m_splitViewAction) {
            QSignalBlocker blocker(m_splitViewAction);
            m_splitViewAction->setChecked(state.splitEnabled);
        }
        if (state.splitEnabled) {
            m_splitPane->show();
            m_splitPane->setViewMode(
                state.splitViewMode >= 0
                    ? state.splitViewMode
                    : m_directoryViewMode);
            m_splitPane->setSortState(
                state.splitSortKey,
                state.splitSortAscending);
            m_splitPane->setCurrentUrl(
                state.splitUrl.isValid()
                    ? state.splitUrl
                    : state.currentUrl,
                true);
        } else {
            m_splitPane->hide();
            setActivePane(PaneId::Primary);
        }
        loadLocation(state.currentUrl);
        m_tabRestoreInProgress = false;
        syncActiveTabState();
    }

    void closeTab(int index)
    {
        if (index < 0 || index >= m_tabs.size()) {
            return;
        }

        if (m_tabs.size() == 1) {
            // Keep one usable tab instead of leaving the window without a view.
            navigateTo(kThisPcUrl, true);
            return;
        }

        if (index == m_activeTab) {
            syncActiveTabState();
        }

        m_closedTabs.push_back(m_tabs.at(index));
        if (m_closedTabs.size() > 20) {
            m_closedTabs.removeFirst();
        }

        const bool wasActive = index == m_activeTab;
        int nextIndex = m_activeTab;

        if (index < m_activeTab) {
            --nextIndex;
        } else if (wasActive) {
            nextIndex = qMin(index, m_tabs.size() - 2);
        }

        m_tabChangeInProgress = true;
        m_tabs.removeAt(index);
        m_tabBar->removeTab(index);
        m_tabChangeInProgress = false;

        if (wasActive) {
            m_activeTab = -1;
            switchToTab(nextIndex);
        } else {
            m_activeTab = nextIndex;
            m_tabChangeInProgress = true;
            m_tabBar->setCurrentIndex(m_activeTab);
            m_tabChangeInProgress = false;
            updateActiveTabPresentation();
        }
    }

    void closeOtherTabs(int keepIndex)
    {
        if (keepIndex < 0 || keepIndex >= m_tabs.size()) {
            return;
        }

        if (keepIndex == m_activeTab) {
            syncActiveTabState();
        }

        const TabState kept = m_tabs.at(keepIndex);

        for (int i = m_tabs.size() - 1; i >= 0; --i) {
            if (i == keepIndex) {
                continue;
            }
            m_closedTabs.push_back(m_tabs.at(i));
        }

        while (m_closedTabs.size() > 20) {
            m_closedTabs.removeFirst();
        }

        m_tabChangeInProgress = true;
        while (m_tabBar->count() > 0) {
            m_tabBar->removeTab(0);
        }
        m_tabs.clear();
        m_tabs.push_back(kept);
        m_tabBar->addTab(
            tabIconForUrl(kept.currentUrl),
            tabTitleForUrl(kept.currentUrl));
        m_tabBar->setTabToolTip(
            0,
            urlForDisplay(kept.currentUrl));
        m_tabBar->setCurrentIndex(0);
        m_tabChangeInProgress = false;

        m_activeTab = -1;
        switchToTab(0);
    }

    void showTabContextMenu(const QPoint &pos)
    {
        const int index = m_tabBar->tabAt(pos);
        QMenu menu(this);

        QAction *newTab = menu.addAction(
            themedIcon(QStringLiteral("tab-new")),
            trLocal("Nowa karta", "New tab"));
        newTab->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));

        QAction *duplicate = nullptr;
        QAction *close = nullptr;
        QAction *closeOthers = nullptr;

        if (index >= 0) {
            duplicate = menu.addAction(
                themedIcon(QStringLiteral("edit-copy")),
                trLocal("Duplikuj kartę", "Duplicate tab"));

            menu.addSeparator();

            close = menu.addAction(
                themedIcon(QStringLiteral("window-close")),
                trLocal("Zamknij kartę", "Close tab"));
            close->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));

            closeOthers = menu.addAction(
                trLocal(
                    "Zamknij pozostałe karty",
                    "Close other tabs"));
            closeOthers->setEnabled(m_tabs.size() > 1);
        }

        menu.addSeparator();

        QAction *reopen = menu.addAction(
            themedIcon(QStringLiteral("edit-undo")),
            trLocal(
                "Otwórz ponownie zamkniętą kartę",
                "Reopen closed tab"));
        reopen->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Shift+T")));
        reopen->setEnabled(!m_closedTabs.isEmpty());

        QAction *chosen = menu.exec(
            m_tabBar->mapToGlobal(pos));

        if (chosen == newTab) {
            createNewTab(kThisPcUrl, true);
        } else if (duplicate && chosen == duplicate) {
            duplicateTab(index);
        } else if (close && chosen == close) {
            closeTab(index);
        } else if (closeOthers && chosen == closeOthers) {
            closeOtherTabs(index);
        } else if (chosen == reopen) {
            reopenClosedTab();
        }
    }

    void buildTabShortcuts()
    {
        auto *newWindow = new QAction(this);
        newWindow->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+N")));
        newWindow->setShortcutContext(Qt::WindowShortcut);
        addAction(newWindow);
        connect(
            newWindow,
            &QAction::triggered,
            this,
            [this] {
                openInNewWindow(m_currentUrl);
            });

        auto *newTab = new QAction(this);
        newTab->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+T")));
        newTab->setShortcutContext(Qt::WindowShortcut);
        addAction(newTab);
        connect(
            newTab,
            &QAction::triggered,
            this,
            [this] { createNewTab(kThisPcUrl, true); });

        auto *closeCurrent = new QAction(this);
        closeCurrent->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+W")));
        closeCurrent->setShortcutContext(Qt::WindowShortcut);
        addAction(closeCurrent);
        connect(
            closeCurrent,
            &QAction::triggered,
            this,
            [this] { closeTab(m_activeTab); });

        auto *reopen = new QAction(this);
        reopen->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Shift+T")));
        reopen->setShortcutContext(Qt::WindowShortcut);
        addAction(reopen);
        connect(
            reopen,
            &QAction::triggered,
            this,
            &ThisPcWindow::reopenClosedTab);

        auto *nextTab = new QAction(this);
        nextTab->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Tab")));
        nextTab->setShortcutContext(Qt::WindowShortcut);
        addAction(nextTab);
        connect(
            nextTab,
            &QAction::triggered,
            this,
            [this] {
                if (m_tabBar->count() < 2) {
                    return;
                }
                const int next =
                    (m_tabBar->currentIndex() + 1)
                    % m_tabBar->count();
                m_tabBar->setCurrentIndex(next);
            });

        auto *previousTab = new QAction(this);
        previousTab->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Shift+Tab")));
        previousTab->setShortcutContext(Qt::WindowShortcut);
        addAction(previousTab);
        connect(
            previousTab,
            &QAction::triggered,
            this,
            [this] {
                if (m_tabBar->count() < 2) {
                    return;
                }
                const int previous =
                    (m_tabBar->currentIndex()
                     + m_tabBar->count() - 1)
                    % m_tabBar->count();
                m_tabBar->setCurrentIndex(previous);
            });
    }

    void navigateTo(const QUrl &rawUrl, bool addHistory)
    {
        if (!rawUrl.isValid()) {
            return;
        }

        const QUrl url = normalizedUrl(rawUrl);

        if (addHistory) {
            if (m_historyIndex >= 0
                && m_historyIndex < m_history.size()
                && sameLocation(m_history.at(m_historyIndex), url)) {
                loadLocation(url);
                return;
            }

            while (m_history.size() > m_historyIndex + 1) {
                m_history.removeLast();
            }

            m_history.push_back(url);
            m_historyIndex = m_history.size() - 1;
        }

        loadLocation(url);
    }

    void loadLocation(const QUrl &url)
    {
        const bool changedLocation =
            !sameLocation(m_currentUrl, url);

        if (changedLocation && m_searchInProgress) {
            cancelActiveSearch(false);
        }

        m_currentUrl = url;

        if (m_adminBanner) {
            m_adminBanner->setVisible(
                isAdminUrl(url));
        }

        if (changedLocation && m_searchEdit) {
            const QString embeddedSearch =
                searchQueryFromUrl(url);

            m_searchEdit->blockSignals(true);

            if (!embeddedSearch.isEmpty()) {
                m_searchEdit->setText(embeddedSearch);
                m_searchQuery.clear();
            } else {
                m_searchEdit->clear();
                m_searchQuery.clear();
            }

            m_searchEdit->blockSignals(false);
        }

        if (isSearchLocation(url)) {
            m_searchScopeMode =
                std::clamp(
                    searchIntParameter(
                        url,
                        QStringLiteral("scope"),
                        2),
                    0,
                    2);
        } else if (sameLocation(
                       url,
                       kThisPcUrl)) {
            m_searchScopeMode = 2;
            m_searchTypeFilter = 0;
            m_searchDateFilter = 0;
            m_searchSizeFilter = 0;
        } else {
            m_searchScopeMode = 0;
            m_searchTypeFilter = 0;
            m_searchDateFilter = 0;
            m_searchSizeFilter = 0;
        }

        updateSearchControls();

        if (m_searchEdit) {
            m_searchEdit->setVisible(true);
            m_searchEdit->setEnabled(true);

            if (sameLocation(url, kThisPcUrl)) {
                m_searchEdit->setPlaceholderText(
                    trLocal(
                        "Szukaj na tym komputerze",
                        "Search this computer"));
            } else if (isSearchLocation(url)) {
                m_searchEdit->setPlaceholderText(
                    trLocal(
                        "Nowe wyszukiwanie",
                        "New search"));
            } else {
                m_searchEdit->setPlaceholderText(
                    isPolish()
                        ? QStringLiteral("Szukaj w: %1")
                            .arg(displayNameForLocation(url))
                        : QStringLiteral("Search in: %1")
                            .arg(displayNameForLocation(url)));
            }
        }

        if (m_directoryJob) {
            m_directoryJob->kill();
            m_directoryJob = nullptr;
        }

        if (sameLocation(url, kThisPcUrl)) {
            m_contentStack->setCurrentWidget(m_homePage);
            if (m_directoryDetails) {
                m_directoryDetails->setColumnHidden(
                    4,
                    true);
            }
            statusBar()->showMessage(
                trLocal("Ten komputer", "This PC"));
        } else {
            m_contentStack->setCurrentWidget(m_directoryPage);

            if (isSearchLocation(url)) {
                loadSearchLocation(url);
            } else {
                m_directoryDetails->setColumnHidden(
                    4,
                    true);
                loadDirectory(url);
            }
        }

        rebuildBreadcrumbs();
        updateNavigationActions();
        updateSidebarCurrent();
        updateFileActionStates();
        syncActiveTabState();
        updateActiveTabPresentation();
    }

    void loadDirectory(const QUrl &url)
    {
        m_pendingFiles.clear();
        m_directoryList->clear();
        m_directoryDetails->clear();
        m_directoryList->setDropDirectory(url);
        m_directoryDetails->setDropDirectory(url);

        m_directoryTitle->setText(displayNameForLocation(url));
        m_directoryStatus->setText(
            trLocal("Wczytywanie…", "Loading…"));

        KIO::ListJob *job = KIO::listDir(
            url,
            KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        m_directoryJob = job;

        connect(
            job,
            &KIO::ListJob::entries,
            this,
            [this, url, job](
                KIO::Job *,
                const KIO::UDSEntryList &entries) {
            if (m_directoryJob != job) {
                return;
            }

            for (const KIO::UDSEntry &entry : entries) {
                QString name =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_DISPLAY_NAME);

                if (name.isEmpty()) {
                    name =
                        entry.stringValue(
                            KIO::UDSEntry::UDS_NAME);
                }

                const QString rawName =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_NAME);

                if (name.isEmpty()
                    || rawName == QStringLiteral(".")
                    || rawName == QStringLiteral("..")
                    || (!m_showHiddenFiles
                        && rawName.startsWith(QLatin1Char('.')))) {
                    continue;
                }

                FileInfo file;
                file.name = name;
                file.mimeType =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_MIME_TYPE);
                file.iconName =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_ICON_NAME);
                file.url = childUrlForEntry(url, entry);
                file.isDir = entryIsDirectory(entry);
                file.size =
                    entry.numberValue(
                        KIO::UDSEntry::UDS_SIZE,
                        -1);
                file.modificationTime =
                    entry.numberValue(
                        KIO::UDSEntry::UDS_MODIFICATION_TIME,
                        0);

                m_pendingFiles.push_back(file);
            }
        });

        connect(
            job,
            &KJob::result,
            this,
            [this, job](KJob *) {
            if (m_directoryJob != job) {
                return;
            }

            m_directoryJob = nullptr;

            if (job->error()) {
                m_directoryStatus->setText(
                    trLocal(
                        "Nie udało się otworzyć tej lokalizacji.",
                        "Could not open this location."));

                statusBar()->showMessage(
                    job->errorString(),
                    6000);
                return;
            }

            renderDirectoryItems();
        });
    }

    bool fileMatchesSearch(const FileInfo &file) const
    {
        if (m_searchQuery.isEmpty()) {
            return true;
        }

        return file.name.contains(
                m_searchQuery,
                Qt::CaseInsensitive)
            || file.mimeType.contains(
                m_searchQuery,
                Qt::CaseInsensitive);
    }

    QIcon iconForFile(
        const FileInfo &file,
        QMimeDatabase &mimeDatabase)
    {
        const QMimeType mime =
            resolvedMimeType(file, mimeDatabase);

        QString iconName = file.iconName;

        // UDS_ICON_NAME is often empty on plain/local mounts. MIME-based icons
        // give PNG/PDF/archive/office/etc files the same type-specific look as
        // a normal KDE file manager.
        if (iconName.isEmpty()
            || iconName == QStringLiteral("text-x-generic")) {
            if (mime.isValid() && !mime.isDefault()) {
                iconName = mime.iconName();
                if (iconName.isEmpty()) {
                    iconName = mime.genericIconName();
                }
            }
        }

        if (iconName.isEmpty()) {
            iconName =
                file.isDir
                    ? QStringLiteral("folder")
                    : QStringLiteral("unknown");
        }

        QIcon fallback = themedIcon(
            iconName,
            file.isDir
                ? QStringLiteral("folder")
                : QStringLiteral("text-x-generic"));

        const QString mimeName =
            mime.isValid() ? mime.name() : QString();

        if (!m_thumbnailsEnabled
            || file.isDir
            || !file.url.isLocalFile()
            || !mimeName.startsWith(
                QStringLiteral("image/"))) {
            return fallback;
        }

        constexpr qint64 maxThumbnailFileSize =
            64LL * 1024LL * 1024LL;

        if (file.size > maxThumbnailFileSize) {
            return fallback;
        }

        const QString path = file.url.toLocalFile();
        const QString cacheKey =
            QStringLiteral("%1|%2|%3")
                .arg(path)
                .arg(file.modificationTime)
                .arg(file.size);

        const auto cached =
            m_thumbnailCache.constFind(cacheKey);
        if (cached != m_thumbnailCache.constEnd()) {
            return cached.value();
        }

        QImageReader reader(path);
        reader.setAutoTransform(true);

        const QSize sourceSize = reader.size();
        const QSize targetSize(128, 128);
        if (sourceSize.isValid()
            && (sourceSize.width() > targetSize.width()
                || sourceSize.height() > targetSize.height())) {
            reader.setScaledSize(
                sourceSize.scaled(
                    targetSize,
                    Qt::KeepAspectRatio));
        }

        const QImage image = reader.read();
        if (image.isNull()) {
            return fallback;
        }

        QPixmap pixmap = QPixmap::fromImage(image);
        pixmap = pixmap.scaled(
            targetSize,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation);

        QIcon icon(pixmap);
        m_thumbnailCache.insert(cacheKey, icon);
        return icon;
    }

    void renderDirectoryItems()
    {
        auto compareText =
            [](const QString &a, const QString &b) {
            return a.localeAwareCompare(b);
        };

        std::sort(
            m_pendingFiles.begin(),
            m_pendingFiles.end(),
            [this, compareText](
                const FileInfo &a,
                const FileInfo &b) {
            // Keep folders together before files, like Explorer/Dolphin.
            if (a.isDir != b.isDir) {
                return a.isDir;
            }

            int comparison = 0;

            switch (m_sortKey) {
            case 1:
                comparison =
                    compareText(a.mimeType, b.mimeType);
                break;
            case 2:
                if (a.size < b.size) {
                    comparison = -1;
                } else if (a.size > b.size) {
                    comparison = 1;
                }
                break;
            case 3:
                if (a.modificationTime < b.modificationTime) {
                    comparison = -1;
                } else if (a.modificationTime > b.modificationTime) {
                    comparison = 1;
                }
                break;
            case 0:
            default:
                comparison =
                    compareText(a.name, b.name);
                break;
            }

            if (comparison == 0) {
                comparison =
                    compareText(a.name, b.name);
            }

            return m_sortAscending
                ? comparison < 0
                : comparison > 0;
        });

        m_directoryList->clear();
        m_directoryDetails->clear();

        QMimeDatabase mimeDb;
        int visibleCount = 0;

        for (const FileInfo &file : std::as_const(m_pendingFiles)) {
            if (!fileMatchesSearch(file)) {
                continue;
            }

            if (isSearchLocation(m_currentUrl)
                && !fileMatchesAdvancedSearch(
                    file,
                    mimeDb)) {
                continue;
            }

            ++visibleCount;

            const QIcon icon =
                iconForFile(file, mimeDb);
            const QString typeText =
                fileTypeLabel(file, mimeDb);
            const QString sizeText =
                formatFileSize(file.size, file.isDir);
            const QString modifiedText =
                formatModificationTime(
                    file.modificationTime);
            const QString locationText =
                parentLocationForDisplay(
                    file.url);

            auto *listItem =
                new QListWidgetItem(
                    icon,
                    file.name,
                    m_directoryList);

            listItem->setData(
                Qt::UserRole,
                file.url.toString());
            listItem->setData(
                Qt::UserRole + 1,
                file.isDir);
            listItem->setData(
                Qt::UserRole + 2,
                typeText);
            listItem->setData(
                Qt::UserRole + 3,
                sizeText);
            listItem->setData(
                Qt::UserRole + 4,
                modifiedText);
            listItem->setToolTip(
                QStringLiteral("%1\n%2\n%3")
                    .arg(
                        urlForDisplay(file.url),
                        typeText,
                        modifiedText));

            auto *detailsItem =
                new QTreeWidgetItem(
                    m_directoryDetails,
                    {
                        file.name,
                        typeText,
                        sizeText,
                        modifiedText,
                        locationText
                    });

            detailsItem->setIcon(0, icon);
            detailsItem->setData(
                0,
                Qt::UserRole,
                file.url.toString());
            detailsItem->setData(
                0,
                Qt::UserRole + 1,
                file.isDir);
            detailsItem->setToolTip(
                0,
                urlForDisplay(file.url));
            detailsItem->setTextAlignment(
                2,
                Qt::AlignRight | Qt::AlignVCenter);
        }

        const int count = m_pendingFiles.size();

        if (isSearchLocation(m_currentUrl)) {
            m_searchVisibleCount =
                visibleCount;
            updateSearchStatusLabel();
        } else if (m_searchQuery.isEmpty()) {
            m_directoryStatus->setText(
                isPolish()
                    ? QStringLiteral("%1 elementów").arg(count)
                    : QStringLiteral("%1 items").arg(count));
        } else {
            m_directoryStatus->setText(
                isPolish()
                    ? QStringLiteral("%1 z %2 elementów")
                        .arg(visibleCount)
                        .arg(count)
                    : QStringLiteral("%1 of %2 items")
                        .arg(visibleCount)
                        .arg(count));
        }

        statusBar()->showMessage(
            isSearchLocation(m_currentUrl)
                ? displayNameForLocation(m_currentUrl)
                : urlForDisplay(m_currentUrl));

        updateFileActionStates();
    }

    void activateDirectoryItem(QListWidgetItem *item)
    {
        if (!item) {
            return;
        }

        const QUrl url(
            item->data(Qt::UserRole).toString());

        const bool isDir =
            item->data(Qt::UserRole + 1).toBool();

        if (isDir) {
            navigateTo(url, true);
        } else {
            QDesktopServices::openUrl(url);
        }
    }


    void activateDetailsItem(QTreeWidgetItem *item)
    {
        if (!item) {
            return;
        }

        const QUrl url(
            item->data(0, Qt::UserRole).toString());

        const bool isDir =
            item->data(
                0,
                Qt::UserRole + 1).toBool();

        if (isDir) {
            navigateTo(url, true);
        } else {
            QDesktopServices::openUrl(url);
        }
    }


    void addOpenWithSubmenu(
        QMenu &menu,
        const QList<QUrl> &urls)
    {
        if (urls.isEmpty()) {
            return;
        }

        QMenu *openWithMenu =
            menu.addMenu(
                themedIcon(QStringLiteral("document-open-with"),
                           QStringLiteral("system-run")),
                trLocal("Otwórz za pomocą", "Open with"));

        KFileItemList items;
        items.reserve(urls.size());
        for (const QUrl &url : urls) {
            items.push_back(
                KFileItem(
                    url,
                    KFileItem::NormalMimeTypeDetermination));
        }

        auto *actions =
            new KFileItemActions(openWithMenu);
        actions->setParentWidget(this);
        actions->setItemListProperties(
            KFileItemListProperties(items));
        actions->insertOpenWithActionsTo(
            nullptr,
            openWithMenu,
            {});

        if (openWithMenu->actions().isEmpty()) {
            QAction *none = openWithMenu->addAction(
                trLocal("Brak pasujących aplikacji", "No matching applications"));
            none->setEnabled(false);
        }
    }

    enum class PrintableKind {
        None,
        Image,
        Text,
        Pdf
    };

    PrintableKind printableKindForUrl(
        const QUrl &url,
        bool isDir) const
    {
        if (isDir || !url.isLocalFile()) {
            return PrintableKind::None;
        }

        QMimeDatabase database;
        const QMimeType mime =
            database.mimeTypeForFile(
                url.toLocalFile(),
                QMimeDatabase::MatchContent);

        if (!mime.isValid()) {
            return PrintableKind::None;
        }

        if (mime.name() == QStringLiteral("application/pdf")) {
            return QStandardPaths::findExecutable(
                       QStringLiteral("okular")).isEmpty()
                ? PrintableKind::None
                : PrintableKind::Pdf;
        }

        if (mime.name().startsWith(
                QStringLiteral("image/"))) {
            return PrintableKind::Image;
        }

        if (mime.name().startsWith(
                QStringLiteral("text/"))) {
            return PrintableKind::Text;
        }

        return PrintableKind::None;
    }

    bool canPrintUrl(
        const QUrl &url,
        bool isDir) const
    {
        return printableKindForUrl(url, isDir)
            != PrintableKind::None;
    }

    void printImageUrl(const QUrl &url)
    {
        QImageReader reader(url.toLocalFile());
        reader.setAutoTransform(true);

        const QImage image = reader.read();

        if (image.isNull()) {
            QMessageBox::warning(
                this,
                trLocal("Drukowanie", "Printing"),
                isPolish()
                    ? QStringLiteral(
                        "Nie udało się odczytać obrazu:\n%1")
                        .arg(reader.errorString())
                    : QStringLiteral(
                        "Could not read the image:\n%1")
                        .arg(reader.errorString()));
            return;
        }

        QPrinter printer(QPrinter::HighResolution);
        printer.setDocName(
            QFileInfo(url.toLocalFile()).fileName());

        QPrintDialog dialog(&printer, this);
        dialog.setWindowTitle(
            trLocal("Drukuj obraz", "Print image"));

        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        QPainter painter(&printer);
        if (!painter.isActive()) {
            QMessageBox::warning(
                this,
                trLocal("Drukowanie", "Printing"),
                trLocal(
                    "Nie udało się rozpocząć drukowania.",
                    "Could not start printing."));
            return;
        }

        const QRect pageRect =
            printer.pageLayout().paintRectPixels(
                printer.resolution());

        QSize targetSize =
            image.size();
        targetSize.scale(
            pageRect.size(),
            Qt::KeepAspectRatio);

        const QRect targetRect(
            pageRect.x()
                + (pageRect.width()
                   - targetSize.width()) / 2,
            pageRect.y()
                + (pageRect.height()
                   - targetSize.height()) / 2,
            targetSize.width(),
            targetSize.height());

        painter.drawImage(
            targetRect,
            image);
    }

    void printTextUrl(const QUrl &url)
    {
        QFile file(url.toLocalFile());

        if (!file.open(
                QIODevice::ReadOnly
                | QIODevice::Text)) {
            QMessageBox::warning(
                this,
                trLocal("Drukowanie", "Printing"),
                trLocal(
                    "Nie udało się otworzyć pliku tekstowego.",
                    "Could not open the text file."));
            return;
        }

        // Avoid accidentally loading an enormous log into QTextDocument.
        constexpr qint64 kMaxTextPrintBytes =
            32LL * 1024LL * 1024LL;

        if (file.size() > kMaxTextPrintBytes) {
            QMessageBox::warning(
                this,
                trLocal("Drukowanie", "Printing"),
                trLocal(
                    "Plik tekstowy jest zbyt duży do bezpośredniego drukowania (limit 32 MiB).",
                    "The text file is too large for direct printing (32 MiB limit)."));
            return;
        }

        const QString text =
            QString::fromUtf8(file.readAll());

        QTextDocument document;
        document.setPlainText(text);

        QPrinter printer(QPrinter::HighResolution);
        printer.setDocName(
            QFileInfo(url.toLocalFile()).fileName());

        QPrintDialog dialog(&printer, this);
        dialog.setWindowTitle(
            trLocal(
                "Drukuj dokument tekstowy",
                "Print text document"));

        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        document.print(&printer);
    }

    void printUrl(const QUrl &url)
    {
        const PrintableKind kind =
            printableKindForUrl(url, false);

        switch (kind) {
        case PrintableKind::Image:
            printImageUrl(url);
            return;

        case PrintableKind::Text:
            printTextUrl(url);
            return;

        case PrintableKind::Pdf: {
            const QString okular =
                QStandardPaths::findExecutable(
                    QStringLiteral("okular"));

            if (okular.isEmpty()
                || !QProcess::startDetached(
                    okular,
                    {
                        QStringLiteral("--print"),
                        url.toLocalFile()
                    })) {
                QMessageBox::warning(
                    this,
                    trLocal("Drukowanie", "Printing"),
                    trLocal(
                        "Nie udało się uruchomić okna drukowania PDF w Okularze.",
                        "Could not start the PDF print dialog in Okular."));
            }
            return;
        }

        case PrintableKind::None:
        default:
            QMessageBox::information(
                this,
                trLocal("Drukowanie", "Printing"),
                trLocal(
                    "Ten typ pliku nie ma jeszcze obsługi drukowania.",
                    "This file type does not have printing support yet."));
            return;
        }
    }


    bool isLocalImageUrl(
        const QUrl &url,
        bool isDir = false) const
    {
        if (isDir || !url.isLocalFile()) {
            return false;
        }

        QMimeDatabase database;
        const QMimeType mime =
            database.mimeTypeForFile(
                url.toLocalFile(),
                QMimeDatabase::MatchContent);

        return mime.isValid()
            && mime.name().startsWith(
                QStringLiteral("image/"));
    }

    bool canSetWallpaper(
        const QUrl &url,
        bool isDir) const
    {
        return isLocalImageUrl(url, isDir)
            && !QStandardPaths::findExecutable(
                    QStringLiteral(
                        "plasma-apply-wallpaperimage"))
                    .isEmpty();
    }

    void setAsDesktopWallpaper(
        const QUrl &url)
    {
        if (!isLocalImageUrl(url, false)) {
            return;
        }

        const QString executable =
            QStandardPaths::findExecutable(
                QStringLiteral(
                    "plasma-apply-wallpaperimage"));

        if (executable.isEmpty()) {
            QMessageBox::information(
                this,
                trLocal(
                    "Tło pulpitu",
                    "Desktop wallpaper"),
                trLocal(
                    "Nie znaleziono narzędzia plasma-apply-wallpaperimage.",
                    "plasma-apply-wallpaperimage was not found."));
            return;
        }

        if (!QProcess::startDetached(
                executable,
                {url.toLocalFile()})) {
            QMessageBox::warning(
                this,
                trLocal(
                    "Tło pulpitu",
                    "Desktop wallpaper"),
                trLocal(
                    "Nie udało się ustawić obrazu jako tła pulpitu.",
                    "Could not set the image as the desktop wallpaper."));
            return;
        }

        statusBar()->showMessage(
            trLocal(
                "Przekazano obraz do ustawienia jako tło pulpitu.",
                "The image was sent to Plasma as the desktop wallpaper."),
            4000);
    }

    bool allUrlsAreLocalFiles(
        const QList<QUrl> &urls,
        bool allowDirectories) const
    {
        if (urls.isEmpty()) {
            return false;
        }

        for (const QUrl &url : urls) {
            if (!url.isLocalFile()) {
                return false;
            }

            if (!allowDirectories
                && QFileInfo(
                    url.toLocalFile()).isDir()) {
                return false;
            }
        }

        return true;
    }

    void copySelectionToDirectory(
        const QList<QUrl> &urls,
        const QString &directory,
        const QString &successMessage)
    {
        if (urls.isEmpty()
            || directory.isEmpty()) {
            return;
        }

        QDir target(directory);
        if (!target.exists()
            && !QDir().mkpath(directory)) {
            QMessageBox::warning(
                this,
                trLocal("Wyślij do", "Send to"),
                trLocal(
                    "Nie udało się utworzyć katalogu docelowego.",
                    "Could not create the destination directory."));
            return;
        }

        auto *job =
            KIO::copy(
                urls,
                QUrl::fromLocalFile(directory),
                KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        recordCopyJobForUndo(job);

        watchFileOperation(
            job,
            successMessage,
            false,
            trLocal("Kopiowanie", "Copying"));
    }

    void sendSelectionByEmail(
        const QList<QUrl> &urls)
    {
        if (!allUrlsAreLocalFiles(
                urls,
                false)) {
            return;
        }

        const QString executable =
            QStandardPaths::findExecutable(
                QStringLiteral("xdg-email"));

        if (executable.isEmpty()) {
            QMessageBox::information(
                this,
                trLocal("Wyślij e-mailem", "Send by e-mail"),
                trLocal(
                    "Nie znaleziono narzędzia xdg-email.",
                    "xdg-email was not found."));
            return;
        }

        QStringList arguments;

        for (const QUrl &url : urls) {
            arguments
                << QStringLiteral("--attach")
                << url.toLocalFile();
        }

        arguments
            << QStringLiteral("--subject")
            << trLocal(
                "Pliki z Ten komputer",
                "Files from This PC");

        if (!QProcess::startDetached(
                executable,
                arguments)) {
            QMessageBox::warning(
                this,
                trLocal("Wyślij e-mailem", "Send by e-mail"),
                trLocal(
                    "Nie udało się uruchomić domyślnego programu pocztowego.",
                    "Could not start the default e-mail application."));
        }
    }

    void sendSelectionByBluetooth(
        const QList<QUrl> &urls)
    {
        if (!allUrlsAreLocalFiles(
                urls,
                false)) {
            return;
        }

        const QString executable =
            QStandardPaths::findExecutable(
                QStringLiteral(
                    "bluedevil-sendfile"));

        if (executable.isEmpty()) {
            QMessageBox::information(
                this,
                trLocal("Bluetooth", "Bluetooth"),
                trLocal(
                    "Nie znaleziono bluedevil-sendfile. Zainstaluj BlueDevil, aby wysyłać pliki przez Bluetooth.",
                    "bluedevil-sendfile was not found. Install BlueDevil to send files over Bluetooth."));
            return;
        }

        QStringList arguments;

        // BlueDevil's current command-line interface accepts one -f/--files
        // option per file and opens the device-selection wizard when no
        // receiving-device option is supplied.
        for (const QUrl &url : urls) {
            arguments
                << QStringLiteral("-f")
                << url.toLocalFile();
        }

        if (!QProcess::startDetached(
                executable,
                arguments)) {
            QMessageBox::warning(
                this,
                trLocal("Bluetooth", "Bluetooth"),
                trLocal(
                    "Nie udało się uruchomić kreatora wysyłania Bluetooth.",
                    "Could not start the Bluetooth file transfer wizard."));
        }
    }

    bool selectionHasCommonParent(
        const QList<QUrl> &urls,
        QString *parentPath = nullptr) const
    {
        if (!allUrlsAreLocalFiles(
                urls,
                true)) {
            return false;
        }

        QString common;

        for (const QUrl &url : urls) {
            const QString parent =
                QFileInfo(
                    url.toLocalFile())
                    .absolutePath();

            if (common.isEmpty()) {
                common = parent;
            } else if (
                QDir::cleanPath(common)
                != QDir::cleanPath(parent)) {
                return false;
            }
        }

        if (parentPath) {
            *parentPath = common;
        }

        return !common.isEmpty();
    }

    void createZipFromSelection(
        const QList<QUrl> &urls)
    {
        QString parent;

        if (!selectionHasCommonParent(
                urls,
                &parent)) {
            QMessageBox::information(
                this,
                trLocal(
                    "Skompresowany ZIP",
                    "Compressed ZIP"),
                trLocal(
                    "Tworzenie ZIP jest obecnie dostępne tylko dla elementów znajdujących się w tym samym katalogu.",
                    "ZIP creation is currently available only for items in the same directory."));
            return;
        }

        const QString zipExecutable =
            QStandardPaths::findExecutable(
                QStringLiteral("zip"));

        if (zipExecutable.isEmpty()) {
            QMessageBox::information(
                this,
                trLocal(
                    "Skompresowany ZIP",
                    "Compressed ZIP"),
                trLocal(
                    "Nie znaleziono programu „zip”.",
                    "The “zip” program was not found."));
            return;
        }

        const QString suggested =
            QDir(parent).filePath(
                trLocal(
                    "Archiwum.zip",
                    "Archive.zip"));

        QString archivePath =
            QFileDialog::getSaveFileName(
                this,
                trLocal(
                    "Utwórz archiwum ZIP",
                    "Create ZIP archive"),
                suggested,
                trLocal(
                    "Archiwa ZIP (*.zip)",
                    "ZIP archives (*.zip)"));

        if (archivePath.isEmpty()) {
            return;
        }

        if (!archivePath.endsWith(
                QStringLiteral(".zip"),
                Qt::CaseInsensitive)) {
            archivePath +=
                QStringLiteral(".zip");
        }

        QStringList arguments;
        arguments << QStringLiteral("-r")
                  << archivePath;

        for (const QUrl &url : urls) {
            arguments
                << QFileInfo(
                    url.toLocalFile())
                    .fileName();
        }

        if (!QProcess::startDetached(
                zipExecutable,
                arguments,
                parent)) {
            QMessageBox::warning(
                this,
                trLocal(
                    "Skompresowany ZIP",
                    "Compressed ZIP"),
                trLocal(
                    "Nie udało się uruchomić tworzenia archiwum ZIP.",
                    "Could not start ZIP archive creation."));
            return;
        }

        statusBar()->showMessage(
            isPolish()
                ? QStringLiteral(
                    "Tworzenie archiwum: %1")
                    .arg(archivePath)
                : QStringLiteral(
                    "Creating archive: %1")
                    .arg(archivePath),
            5000);
    }

    void addSendToSubmenu(
        QMenu &menu,
        const QList<QUrl> &urls)
    {
        if (urls.isEmpty()) {
            return;
        }

        QMenu *sendMenu =
            menu.addMenu(
                themedIcon(
                    QStringLiteral("document-send")),
                trLocal(
                    "Wyślij do",
                    "Send to"));

        const QString desktop =
            QStandardPaths::writableLocation(
                QStandardPaths::DesktopLocation);

        QAction *desktopAction =
            sendMenu->addAction(
                themedIcon(
                    QStringLiteral("user-desktop")),
                trLocal("Pulpit", "Desktop"));
        desktopAction->setEnabled(
            !desktop.isEmpty());

        connect(
            desktopAction,
            &QAction::triggered,
            this,
            [this, urls, desktop] {
                copySelectionToDirectory(
                    urls,
                    desktop,
                    trLocal(
                        "Skopiowano na Pulpit",
                        "Copied to Desktop"));
            });

        const QString documents =
            QStandardPaths::writableLocation(
                QStandardPaths::DocumentsLocation);

        QAction *documentsAction =
            sendMenu->addAction(
                themedIcon(
                    QStringLiteral("folder-documents")),
                trLocal(
                    "Dokumenty",
                    "Documents"));
        documentsAction->setEnabled(
            !documents.isEmpty());

        connect(
            documentsAction,
            &QAction::triggered,
            this,
            [this, urls, documents] {
                copySelectionToDirectory(
                    urls,
                    documents,
                    trLocal(
                        "Skopiowano do Dokumentów",
                        "Copied to Documents"));
            });

        sendMenu->addSeparator();

        const bool plainLocalFiles =
            allUrlsAreLocalFiles(
                urls,
                false);

        QAction *bluetoothAction =
            sendMenu->addAction(
                themedIcon(
                    QStringLiteral(
                        "preferences-system-bluetooth")),
                trLocal(
                    "Bluetooth…",
                    "Bluetooth…"));
        bluetoothAction->setEnabled(
            plainLocalFiles
            && !QStandardPaths::findExecutable(
                    QStringLiteral(
                        "bluedevil-sendfile"))
                    .isEmpty());

        connect(
            bluetoothAction,
            &QAction::triggered,
            this,
            [this, urls] {
                sendSelectionByBluetooth(urls);
            });

        QAction *emailAction =
            sendMenu->addAction(
                themedIcon(
                    QStringLiteral("mail-send")),
                trLocal(
                    "E-mail…",
                    "E-mail…"));
        emailAction->setEnabled(
            plainLocalFiles
            && !QStandardPaths::findExecutable(
                    QStringLiteral(
                        "xdg-email"))
                    .isEmpty());

        connect(
            emailAction,
            &QAction::triggered,
            this,
            [this, urls] {
                sendSelectionByEmail(urls);
            });

        sendMenu->addSeparator();

        QString commonParent;
        QAction *zipAction =
            sendMenu->addAction(
                themedIcon(
                    QStringLiteral("package-x-generic")),
                trLocal(
                    "Skompresowany plik ZIP…",
                    "Compressed ZIP file…"));
        zipAction->setEnabled(
            selectionHasCommonParent(
                urls,
                &commonParent)
            && !QStandardPaths::findExecutable(
                    QStringLiteral("zip"))
                    .isEmpty());

        connect(
            zipAction,
            &QAction::triggered,
            this,
            [this, urls] {
                createZipFromSelection(urls);
            });
    }


    void addViewSubmenu(QMenu &menu)
    {
        const auto pane = paneContext().id;
        const bool split = pane == PaneId::Split;
        QMenu *viewMenu =
            menu.addMenu(
                themedIcon(QStringLiteral("view-list-icons")),
                trLocal("Widok", "View"));

        auto *group =
            new QActionGroup(viewMenu);
        group->setExclusive(true);

        struct ViewDef {
            int mode;
            const char *pl;
            const char *en;
            const char *icon;
        };

        const ViewDef views[] = {
            {0, "Ikony", "Icons", "view-list-icons"},
            {1, "Lista", "List", "view-list-text"},
            {2, "Szczegóły", "Details", "view-list-details"},
        };

        for (const ViewDef &def : views) {
            QAction *action = viewMenu->addAction(
                themedIcon(QString::fromLatin1(def.icon)),
                trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setChecked(
                (split ? m_splitPane->viewMode() : m_directoryViewMode) == def.mode);
            group->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, pane, mode = def.mode] {
                    if (pane == PaneId::Split) m_splitPane->setViewMode(mode);
                    else setDirectoryViewMode(mode);
                });
        }

        viewMenu->addSeparator();

        QAction *hidden = viewMenu->addAction(
            themedIcon(QStringLiteral("view-hidden")),
            trLocal("Ukryte elementy", "Hidden items"));
        hidden->setCheckable(true);
        hidden->setChecked(m_showHiddenFiles);
        connect(
            hidden,
            &QAction::toggled,
            this,
            &ThisPcWindow::setShowHiddenFiles);

        QAction *thumbnails = viewMenu->addAction(
            themedIcon(QStringLiteral("view-preview")),
            trLocal("Miniatury obrazów", "Image thumbnails"));
        thumbnails->setCheckable(true);
        thumbnails->setChecked(m_thumbnailsEnabled);
        connect(
            thumbnails,
            &QAction::toggled,
            this,
            &ThisPcWindow::setThumbnailsEnabled);
    }

    void addSortSubmenu(QMenu &menu)
    {
        const auto pane = paneContext().id;
        const bool split = pane == PaneId::Split;
        QMenu *sortMenu =
            menu.addMenu(
                themedIcon(
                    (split ? m_splitPane->sortAscending() : m_sortAscending)
                        ? QStringLiteral("view-sort-ascending")
                        : QStringLiteral("view-sort-descending")),
                trLocal("Sortuj", "Sort"));

        auto *keyGroup =
            new QActionGroup(sortMenu);
        keyGroup->setExclusive(true);

        struct SortDef {
            int key;
            const char *pl;
            const char *en;
        };

        const SortDef sorts[] = {
            {0, "Nazwa", "Name"},
            {1, "Typ", "Type"},
            {2, "Rozmiar", "Size"},
            {3, "Data modyfikacji", "Date modified"},
        };

        for (const SortDef &def : sorts) {
            QAction *action =
                sortMenu->addAction(
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setChecked(
                (split ? m_splitPane->sortKey() : m_sortKey) == def.key);
            keyGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, pane, key = def.key] {
                    if (pane == PaneId::Split) m_splitPane->setSortState(key, m_splitPane->sortAscending());
                    else setSortKey(key);
                });
        }

        sortMenu->addSeparator();

        auto *directionGroup =
            new QActionGroup(sortMenu);
        directionGroup->setExclusive(true);

        QAction *ascending =
            sortMenu->addAction(
                themedIcon(QStringLiteral("view-sort-ascending")),
                trLocal("Rosnąco", "Ascending"));
        ascending->setCheckable(true);
        ascending->setChecked((split ? m_splitPane->sortAscending() : m_sortAscending));
        directionGroup->addAction(ascending);

        QAction *descending =
            sortMenu->addAction(
                themedIcon(QStringLiteral("view-sort-descending")),
                trLocal("Malejąco", "Descending"));
        descending->setCheckable(true);
        descending->setChecked(!(split ? m_splitPane->sortAscending() : m_sortAscending));
        directionGroup->addAction(descending);

        connect(
            ascending,
            &QAction::triggered,
            this,
            [this, pane] {
                if (pane == PaneId::Split) m_splitPane->setSortState(m_splitPane->sortKey(), true);
                else setSortAscending(true);
            });

        connect(
            descending,
            &QAction::triggered,
            this,
            [this, pane] {
                if (pane == PaneId::Split) m_splitPane->setSortState(m_splitPane->sortKey(), false);
                else setSortAscending(false);
            });
    }

    void selectAllDirectoryItems()
    {
        const auto context = paneContext();
        if (context.isDirectory && context.view) context.view->selectAll();
        updateFileActionStates();
    }

    void configureInteractiveCopyJob(KIO::CopyJob *job)
    {
        if (!job) {
            return;
        }

        // KIO's WidgetsAskUserActionHandler is attached through the default
        // widgets UI delegate.  We keep automatic error/warning handling
        // disabled because watchFileOperation() already owns our error UI;
        // conflict/skip/rename questions remain fully interactive.
        if (KJobUiDelegate *delegate =
                KIO::createDefaultJobUiDelegate(
                    KJobUiDelegate::AutoHandlingDisabled,
                    this)) {
            job->setUiDelegate(delegate);
        }
    }

    void pasteClipboardInto(const QUrl &destination)
    {
        if (!destination.isValid()) {
            return;
        }

        const QMimeData *mime =
            QApplication::clipboard()->mimeData();

        if (!mime || !mime->hasUrls()) {
            return;
        }

        const QList<QUrl> urls = mime->urls();

        if (urls.isEmpty()) {
            return;
        }

        const bool cut =
            mime->data(
                QStringLiteral("application/x-kde-cutselection"))
                == QByteArrayLiteral("1");

        KIO::CopyJob *job =
            cut
                ? KIO::move(
                    urls,
                    destination,
                    KIO::HideProgressInfo)
                : KIO::copy(
                    urls,
                    destination,
                    KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        recordCopyJobForUndo(job);

        watchFileOperation(
            job,
            cut
                ? trLocal("Przenoszenie zakończone", "Move completed")
                : trLocal("Kopiowanie zakończone", "Copy completed"),
            cut,
            cut
                ? trLocal("Przenoszenie", "Moving")
                : trLocal("Kopiowanie", "Copying"));
    }

    bool ensureAdminProtocol()
    {
        if (adminProtocolAvailable()) {
            return true;
        }

        QMessageBox::information(
            this,
            trLocal(
                "Tryb administratora",
                "Administrator mode"),
            trLocal(
                "Nie znaleziono workera KDE „admin://”. Zainstaluj pakiet „kio-admin”, a następnie uruchom ponownie aplikację.",
                "The KDE “admin://” worker is not installed. Install the “kio-admin” package and restart the application."));
        return false;
    }

    void openAsAdministrator(
        const QUrl &url,
        bool isDirectory)
    {
        const PaneId pane = paneContext().id;
        if (!ensureAdminProtocol()) {
            return;
        }

        QUrl target = url;

        if (!isDirectory) {
            target =
                containingDirectoryForResult(url);
        }

        const QString localPath =
            localPathForFileOrAdmin(target);

        if (localPath.isEmpty()) {
            QMessageBox::information(
                this,
                trLocal(
                    "Tryb administratora",
                    "Administrator mode"),
                trLocal(
                    "Tryb administratora jest dostępny tylko dla lokalnych ścieżek systemu plików.",
                    "Administrator mode is available only for local filesystem paths."));
            return;
        }

        navigatePane(pane, adminUrlForLocalPath(localPath));
    }

    void showPropertiesDialog(
        const QString &name,
        const QUrl &url,
        bool isDir,
        const QString &typeText,
        const QString &sizeText,
        const QString &modifiedText)
    {
        KFileItem fileItem(
            url,
            KFileItem::NormalMimeTypeDetermination);

        QDialog dialog(this);
        dialog.setWindowTitle(
            trLocal("Właściwości — ", "Properties — ") + name);
        dialog.resize(630, 625);

        QUrl workingUrl = url;
        QString workingName = name;

        const QString physicalPath =
            localPathForFileOrAdmin(url);
        const QString fileSystem =
            filesystemTypeForUrl(url);
        const bool permissionBehaviorMayDependOnMount =
            filesystemMayUseMountControlledPermissions(
                fileSystem);
        const bool readOnlyFileSystem =
            filesystemIsReadOnly(url);

        auto *outer = new QVBoxLayout(&dialog);

        auto *header = new QHBoxLayout;
        auto *iconLabel = new QLabel(&dialog);
        QIcon icon = themedIcon(
            fileItem.iconName().isEmpty()
                ? (isDir
                    ? QStringLiteral("folder")
                    : QStringLiteral("text-x-generic"))
                : fileItem.iconName());
        iconLabel->setPixmap(icon.pixmap(64, 64));
        iconLabel->setFixedSize(72, 72);
        iconLabel->setAlignment(Qt::AlignCenter);

        auto *headerName = new QLabel(name, &dialog);
        QFont headerFont = headerName->font();
        headerFont.setPointSize(
            headerFont.pointSize() + 3);
        headerFont.setBold(true);
        headerName->setFont(headerFont);
        headerName->setWordWrap(true);

        header->addWidget(iconLabel);
        header->addWidget(headerName, 1);
        outer->addLayout(header);

        auto *tabs = new QTabWidget(&dialog);
        outer->addWidget(tabs, 1);

        auto makeValueLabel =
            [&dialog](const QString &value) {
            auto *label = new QLabel(
                value.isEmpty()
                    ? QStringLiteral("—")
                    : value,
                &dialog);
            label->setTextInteractionFlags(
                Qt::TextSelectableByMouse);
            label->setWordWrap(true);
            return label;
        };

        // --------------------------------------------------------------
        // General
        // --------------------------------------------------------------
        auto *general = new QWidget(tabs);
        auto *generalForm =
            new QFormLayout(general);
        generalForm->setFieldGrowthPolicy(
            QFormLayout::AllNonFixedFieldsGrow);

        auto *nameEdit =
            new QLineEdit(name, general);
        nameEdit->setReadOnly(
            !(url.isLocalFile()
              || isAdminUrl(url)));
        generalForm->addRow(
            trLocal("Nazwa:", "Name:"),
            nameEdit);

        QString mimeName = fileItem.mimetype();
        QString friendlyType =
            fileItem.mimeComment();

        if (friendlyType.isEmpty()) {
            friendlyType = typeText;
        }

        if (mimeName.isEmpty() && !isDir) {
            QMimeDatabase db;
            mimeName =
                db.mimeTypeForFile(
                    physicalPath.isEmpty()
                        ? name
                        : physicalPath,
                    QMimeDatabase::MatchExtension)
                    .name();
        }

        QString actualSize = sizeText;
        if (!isDir) {
            actualSize = formatFileSize(
                static_cast<qint64>(
                    fileItem.size()),
                false);
        }

        const QString modified =
            fileItem.time(
                KFileItem::ModificationTime)
                .isValid()
                ? QLocale().toString(
                    fileItem.time(
                        KFileItem::ModificationTime),
                    QLocale::LongFormat)
                : modifiedText;

        const QDateTime creationTime =
            fileItem.time(
                KFileItem::CreationTime);

        const QString created =
            creationTime.isValid()
            && creationTime.date().year() > 1971
                ? QLocale().toString(
                    creationTime,
                    QLocale::LongFormat)
                : trLocal(
                    "Niedostępne",
                    "Unavailable");

        const QString accessed =
            fileItem.time(
                KFileItem::AccessTime).isValid()
                ? QLocale().toString(
                    fileItem.time(
                        KFileItem::AccessTime),
                    QLocale::LongFormat)
                : QStringLiteral("—");

        generalForm->addRow(
            trLocal("Typ:", "Type:"),
            makeValueLabel(friendlyType));
        generalForm->addRow(
            QStringLiteral("MIME:"),
            makeValueLabel(mimeName));
        generalForm->addRow(
            trLocal("Rozmiar:", "Size:"),
            makeValueLabel(actualSize));
        generalForm->addRow(
            trLocal("Zmodyfikowano:", "Modified:"),
            makeValueLabel(modified));
        generalForm->addRow(
            trLocal("Utworzono:", "Created:"),
            makeValueLabel(created));
        generalForm->addRow(
            trLocal("Ostatni dostęp:", "Accessed:"),
            makeValueLabel(accessed));

        if (!physicalPath.isEmpty()) {
            const QFileInfo info(physicalPath);

            generalForm->addRow(
                trLocal("Lokalizacja:", "Location:"),
                makeValueLabel(
                    info.absolutePath()));
            generalForm->addRow(
                trLocal(
                    "Rozszerzenie:",
                    "Extension:"),
                makeValueLabel(info.suffix()));
            generalForm->addRow(
                trLocal("Właściciel:", "Owner:"),
                makeValueLabel(info.owner()));
            generalForm->addRow(
                trLocal("Grupa:", "Group:"),
                makeValueLabel(info.group()));
            generalForm->addRow(
                trLocal(
                    "System plików:",
                    "Filesystem:"),
                makeValueLabel(
                    fileSystem.isEmpty()
                        ? trLocal(
                            "Niedostępne",
                            "Unavailable")
                        : fileSystem));

            QImageReader reader(physicalPath);
            const QSize imageSize =
                reader.size();

            if (imageSize.isValid()) {
                generalForm->addRow(
                    trLocal(
                        "Wymiary obrazu:",
                        "Image dimensions:"),
                    makeValueLabel(
                        QStringLiteral("%1 × %2 px")
                            .arg(
                                imageSize.width())
                            .arg(
                                imageSize.height())));
            }
        }

        generalForm->addRow(
            trLocal("Adres:", "Address:"),
            makeValueLabel(
                urlForDisplay(url)));

        tabs->addTab(
            general,
            themedIcon(
                QStringLiteral(
                    "document-properties")),
            trLocal("Ogólne", "General"));

        // --------------------------------------------------------------
        // Permissions
        // --------------------------------------------------------------
        auto *permissionsPage =
            new QWidget(tabs);
        auto *permissionsLayout =
            new QVBoxLayout(permissionsPage);

        auto *permissionsInfo =
            new QLabel(permissionsPage);
        permissionsInfo->setWordWrap(true);
        permissionsLayout->addWidget(
            permissionsInfo);

        auto *adminUnlockButton =
            new QPushButton(
                themedIcon(
                    QStringLiteral(
                        "security-high")),
                trLocal(
                    "Odblokuj jako administrator",
                    "Unlock as administrator"),
                permissionsPage);
        adminUnlockButton->setVisible(false);
        permissionsLayout->addWidget(
            adminUnlockButton,
            0,
            Qt::AlignLeft);

        auto *permissionsGroup =
            new QGroupBox(
                trLocal(
                    "Uprawnienia POSIX",
                    "POSIX permissions"),
                permissionsPage);

        auto *permGrid =
            new QGridLayout(
                permissionsGroup);

        permGrid->addWidget(
            new QLabel(
                QString(),
                permissionsGroup),
            0, 0);
        permGrid->addWidget(
            new QLabel(
                trLocal("Odczyt", "Read"),
                permissionsGroup),
            0, 1);
        permGrid->addWidget(
            new QLabel(
                trLocal("Zapis", "Write"),
                permissionsGroup),
            0, 2);
        permGrid->addWidget(
            new QLabel(
                trLocal(
                    "Wykonanie",
                    "Execute"),
                permissionsGroup),
            0, 3);

        permGrid->addWidget(
            new QLabel(
                trLocal(
                    "Właściciel",
                    "Owner"),
                permissionsGroup),
            1, 0);
        permGrid->addWidget(
            new QLabel(
                trLocal("Grupa", "Group"),
                permissionsGroup),
            2, 0);
        permGrid->addWidget(
            new QLabel(
                trLocal("Inni", "Others"),
                permissionsGroup),
            3, 0);

        auto *userRead =
            new QCheckBox(permissionsGroup);
        auto *userWrite =
            new QCheckBox(permissionsGroup);
        auto *userExec =
            new QCheckBox(permissionsGroup);
        auto *groupRead =
            new QCheckBox(permissionsGroup);
        auto *groupWrite =
            new QCheckBox(permissionsGroup);
        auto *groupExec =
            new QCheckBox(permissionsGroup);
        auto *otherRead =
            new QCheckBox(permissionsGroup);
        auto *otherWrite =
            new QCheckBox(permissionsGroup);
        auto *otherExec =
            new QCheckBox(permissionsGroup);

        permGrid->addWidget(
            userRead, 1, 1);
        permGrid->addWidget(
            userWrite, 1, 2);
        permGrid->addWidget(
            userExec, 1, 3);
        permGrid->addWidget(
            groupRead, 2, 1);
        permGrid->addWidget(
            groupWrite, 2, 2);
        permGrid->addWidget(
            groupExec, 2, 3);
        permGrid->addWidget(
            otherRead, 3, 1);
        permGrid->addWidget(
            otherWrite, 3, 2);
        permGrid->addWidget(
            otherExec, 3, 3);

        permissionsLayout->addWidget(
            permissionsGroup);

        auto *recursivePermissions =
            new QCheckBox(
                trLocal(
                    "Zastosuj zmiany do wszystkich podkatalogów i ich zawartości",
                    "Apply changes to all subfolders and their contents"),
                permissionsPage);
        recursivePermissions->setVisible(isDir);
        recursivePermissions->setChecked(false);
        recursivePermissions->setToolTip(
            trLocal(
                "Zmienia prawa istniejących elementów wewnątrz folderu. Dla dużych katalogów operacja może potrwać.",
                "Changes permissions of existing items inside the folder. This can take time for large directories."));
        permissionsLayout->addWidget(
            recursivePermissions);

        const QList<QCheckBox *>
            permissionBoxes = {
                userRead,
                userWrite,
                userExec,
                groupRead,
                groupWrite,
                groupExec,
                otherRead,
                otherWrite,
                otherExec
            };

        auto setPermissionBoxesEnabled =
            [&](bool enabled) {
            for (QCheckBox *box :
                 permissionBoxes) {
                box->setEnabled(enabled);
            }
            recursivePermissions->setEnabled(
                enabled && isDir);
        };

        auto setPermissionBoxesFromMode =
            [&](int mode) {
            userRead->setChecked(
                mode & 0400);
            userWrite->setChecked(
                mode & 0200);
            userExec->setChecked(
                mode & 0100);

            groupRead->setChecked(
                mode & 0040);
            groupWrite->setChecked(
                mode & 0020);
            groupExec->setChecked(
                mode & 0010);

            otherRead->setChecked(
                mode & 0004);
            otherWrite->setChecked(
                mode & 0002);
            otherExec->setChecked(
                mode & 0001);
        };

        auto modeFromPermissionBoxes =
            [&]() {
            int mode = 0;

            if (userRead->isChecked()) {
                mode |= 0400;
            }
            if (userWrite->isChecked()) {
                mode |= 0200;
            }
            if (userExec->isChecked()) {
                mode |= 0100;
            }
            if (groupRead->isChecked()) {
                mode |= 0040;
            }
            if (groupWrite->isChecked()) {
                mode |= 0020;
            }
            if (groupExec->isChecked()) {
                mode |= 0010;
            }
            if (otherRead->isChecked()) {
                mode |= 0004;
            }
            if (otherWrite->isChecked()) {
                mode |= 0002;
            }
            if (otherExec->isChecked()) {
                mode |= 0001;
            }

            return mode;
        };

        bool canEditPermissions = false;
        bool adminUnlocked =
            isAdminUrl(workingUrl);
        int currentMode = -1;
        QString permissionOwner;
        QString permissionGroup;
        QString permissionReadError;

        const bool canReadPermissions =
            readKioPermissions(
                workingUrl,
                &currentMode,
                &permissionOwner,
                &permissionGroup,
                &permissionReadError);

        if (canReadPermissions) {
            setPermissionBoxesFromMode(
                currentMode);
        }

        const bool ordinaryLocal =
            url.isLocalFile();
        const bool ownedByCurrentUser =
            ordinaryLocal
            && localEntryOwnedByCurrentUser(
                url);

        if (readOnlyFileSystem) {
            setPermissionBoxesEnabled(false);
            permissionsInfo->setText(
                trLocal(
                    "Ten system plików jest zamontowany tylko do odczytu. Uprawnień nie można zmienić.",
                    "This filesystem is mounted read-only. Permissions cannot be changed."));
        } else if (!canReadPermissions) {
            setPermissionBoxesEnabled(false);
            permissionsInfo->setText(
                isPolish()
                    ? QStringLiteral(
                        "Nie udało się odczytać uprawnień: %1")
                        .arg(permissionReadError)
                    : QStringLiteral(
                        "Could not read permissions: %1")
                        .arg(permissionReadError));

            if (ordinaryLocal && !adminUnlocked) {
                adminUnlockButton->setVisible(true);
            }
        } else if (adminUnlocked) {
            canEditPermissions = true;
            setPermissionBoxesEnabled(true);
            permissionsInfo->setText(
                trLocal(
                    "Uprawnienia są edytowane przez KIO admin://. Zapis może wywołać systemowe okno autoryzacji PolicyKit.",
                    "Permissions are being edited through KIO admin://. Saving may invoke the system PolicyKit authentication dialog."));
        } else if (ownedByCurrentUser) {
            canEditPermissions = true;
            setPermissionBoxesEnabled(true);
            permissionsInfo->setText(
                trLocal(
                    "Jesteś właścicielem tego elementu. Zmiany zostaną zapisane po użyciu „Zastosuj” lub „OK” i zweryfikowane ponownym odczytem z dysku.",
                    "You own this item. Changes are written after pressing Apply or OK and verified by reading the permissions back from disk."));
        } else {
            canEditPermissions = false;
            setPermissionBoxesEnabled(false);

            permissionsInfo->setText(
                isPolish()
                    ? QStringLiteral(
                        "Nie jesteś właścicielem tego elementu%1. Zmiana uprawnień wymaga autoryzacji administratora.")
                        .arg(
                            permissionOwner.isEmpty()
                                ? QString()
                                : QStringLiteral(
                                    " (właściciel: %1)")
                                    .arg(permissionOwner))
                    : QStringLiteral(
                        "You do not own this item%1. Changing permissions requires administrator authorization.")
                        .arg(
                            permissionOwner.isEmpty()
                                ? QString()
                                : QStringLiteral(
                                    " (owner: %1)")
                                    .arg(permissionOwner)));

            adminUnlockButton->setVisible(true);
        }

        if (ordinaryLocal
            && !adminUnlocked
            && !readOnlyFileSystem
            && !ownedByCurrentUser) {
            adminUnlockButton->setVisible(true);
        }

        permissionsLayout->addStretch(1);

        tabs->addTab(
            permissionsPage,
            themedIcon(
                QStringLiteral("security-high")),
            trLocal(
                "Uprawnienia",
                "Permissions"));

        // --------------------------------------------------------------
        // Administrator unlock
        // --------------------------------------------------------------
        connect(
            adminUnlockButton,
            &QPushButton::clicked,
            &dialog,
            [&] {
                if (!ensureAdminProtocol()) {
                    return;
                }

                const QString localPath =
                    localPathForFileOrAdmin(
                        workingUrl);

                if (localPath.isEmpty()) {
                    return;
                }

                const QUrl elevatedUrl =
                    adminUrlForLocalPath(
                        localPath);

                int elevatedMode = -1;
                QString elevatedOwner;
                QString elevatedGroup;
                QString error;

                // This stat is intentional: it verifies that the admin
                // worker is available and triggers PolicyKit authorization.
                if (!readKioPermissions(
                        elevatedUrl,
                        &elevatedMode,
                        &elevatedOwner,
                        &elevatedGroup,
                        &error)) {
                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Autoryzacja administratora",
                            "Administrator authorization"),
                        isPolish()
                            ? QStringLiteral(
                                "Nie udało się odblokować elementu przez admin://:\n%1")
                                .arg(error)
                            : QStringLiteral(
                                "Could not unlock the item through admin://:\n%1")
                                .arg(error));
                    return;
                }

                workingUrl = elevatedUrl;
                adminUnlocked = true;
                canEditPermissions = true;
                currentMode = elevatedMode;
                permissionOwner =
                    elevatedOwner;
                permissionGroup =
                    elevatedGroup;

                setPermissionBoxesFromMode(
                    elevatedMode);
                setPermissionBoxesEnabled(
                    true);
                adminUnlockButton->hide();

                permissionsInfo->setText(
                    trLocal(
                        "Tryb administratora został odblokowany dla tego elementu. Zmiany zostaną wykonane przez KIO admin:// i zweryfikowane po zapisie.",
                        "Administrator mode is unlocked for this item. Changes will be performed through KIO admin:// and verified after saving."));
            });

        // --------------------------------------------------------------
        // Buttons / write-back with verification
        // --------------------------------------------------------------
        auto *buttons =
            new QDialogButtonBox(
                QDialogButtonBox::Ok
                    | QDialogButtonBox::Apply
                    | QDialogButtonBox::Cancel,
                &dialog);
        outer->addWidget(buttons);

        auto applyChanges = [&]() -> bool {
            bool permissionWriteVerifiedThisApply = false;

            if (canEditPermissions
                && !readOnlyFileSystem) {
                const int requestedMode =
                    modeFromPermissionBoxes();
                const bool recursive =
                    isDir
                    && recursivePermissions->isChecked();

                KJob *chmodJob = nullptr;

                if (recursive) {
                    KFileItem rootItem;
                    QString statError;

                    if (!readKioFileItem(
                            workingUrl,
                            &rootItem,
                            &statError)) {
                        QMessageBox::warning(
                            &dialog,
                            trLocal(
                                "Nie udało się rozpocząć zmiany uprawnień",
                                "Could not start permission change"),
                            isPolish()
                                ? QStringLiteral(
                                    "Nie udało się odczytać informacji potrzebnych do zmiany rekurencyjnej:\n%1")
                                    .arg(statError)
                                : QStringLiteral(
                                    "Could not read the information required for a recursive permission change:\n%1")
                                    .arg(statError));
                        return false;
                    }

                    KFileItemList items;
                    items.push_back(rootItem);

                    chmodJob = KIO::chmod(
                        items,
                        requestedMode,
                        0777,
                        QString(),
                        QString(),
                        true,
                        KIO::DefaultFlags);
                } else {
                    chmodJob = KIO::chmod(
                        workingUrl,
                        requestedMode);
                }

                if (!chmodJob->exec()) {
                    if (url.isLocalFile()
                        && !adminUnlocked) {
                        adminUnlockButton->show();
                    }

                    QString failureText =
                        isPolish()
                            ? QStringLiteral(
                                "System odmówił zmiany uprawnień:\n%1\n\nJeżeli element należy do innego użytkownika, użyj „Odblokuj jako administrator”.")
                                .arg(chmodJob->errorString())
                            : QStringLiteral(
                                "The system refused the permission change:\n%1\n\nIf the item belongs to another user, use “Unlock as administrator”.")
                                .arg(chmodJob->errorString());

                    if (permissionBehaviorMayDependOnMount) {
                        failureText +=
                            isPolish()
                                ? QStringLiteral(
                                    "\n\nTen wolumin jest zgłaszany jako „%1”. Jeżeli nawet administrator nie może zmienić praw, sprawdź sposób montowania woluminu. Dla ntfs-3g prawdziwe chmod wymaga konfiguracji obsługującej uprawnienia, np. permissions wraz z poprawnym UserMapping.")
                                    .arg(
                                        fileSystem.isEmpty()
                                            ? QStringLiteral("?")
                                            : fileSystem)
                                : QStringLiteral(
                                    "\n\nThis volume is reported as “%1”. If even administrator mode cannot change the permissions, check the volume mount configuration. With ntfs-3g, real chmod requires a permissions-capable configuration, for example permissions with a valid UserMapping.")
                                    .arg(
                                        fileSystem.isEmpty()
                                            ? QStringLiteral("?")
                                            : fileSystem);
                    }

                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Nie udało się zmienić uprawnień",
                            "Could not change permissions"),
                        failureText);
                    return false;
                }

                int verifiedMode = -1;
                QString verifyError;

                if (!readKioPermissions(
                        workingUrl,
                        &verifiedMode,
                        nullptr,
                        nullptr,
                        &verifyError)) {
                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Nie można zweryfikować uprawnień",
                            "Could not verify permissions"),
                        isPolish()
                            ? QStringLiteral(
                                "Polecenie zmiany zostało wykonane, ale nie udało się ponownie odczytać praw z dysku:\n%1")
                                .arg(
                                    verifyError)
                            : QStringLiteral(
                                "The change operation completed, but the permissions could not be read back from disk:\n%1")
                                .arg(
                                    verifyError));
                    return false;
                }

                setPermissionBoxesFromMode(
                    verifiedMode);
                currentMode = verifiedMode;

                if ((verifiedMode & 0777)
                    != (requestedMode & 0777)) {
                    if (url.isLocalFile()
                        && !adminUnlocked) {
                        adminUnlockButton->show();
                    }

                    QString mismatchText =
                        isPolish()
                            ? QStringLiteral(
                                "System zaakceptował operację, ale po ponownym odczycie prawa nadal mają wartość %1 zamiast %2.")
                                .arg(
                                    QString::number(
                                        verifiedMode & 0777,
                                        8),
                                    QString::number(
                                        requestedMode & 0777,
                                        8))
                            : QStringLiteral(
                                "The operation completed, but after reading the file back its mode is still %1 instead of %2.")
                                .arg(
                                    QString::number(
                                        verifiedMode & 0777,
                                        8),
                                    QString::number(
                                        requestedMode & 0777,
                                        8));

                    if (permissionBehaviorMayDependOnMount) {
                        mismatchText +=
                            isPolish()
                                ? QStringLiteral(
                                    "\n\nNa tym woluminie (%1) sterownik lub opcje montowania nie zachowały żądanej zmiany. Dla ntfs-3g sprawdź, czy wolumin jest montowany z permissions i poprawnym .NTFS-3G/UserMapping; konfiguracje oparte na stałych uid/gid/umask mogą ignorować chmod.")
                                    .arg(
                                        fileSystem.isEmpty()
                                            ? QStringLiteral("?")
                                            : fileSystem)
                                : QStringLiteral(
                                    "\n\nOn this volume (%1), the driver or mount options did not preserve the requested change. With ntfs-3g, check that the volume is mounted with permissions and a valid .NTFS-3G/UserMapping; configurations based on fixed uid/gid/umask can ignore chmod.")
                                    .arg(
                                        fileSystem.isEmpty()
                                            ? QStringLiteral("?")
                                            : fileSystem);
                    } else {
                        mismatchText +=
                            trLocal(
                                "\n\nMoże to oznaczać brak wystarczających uprawnień do tego elementu.",
                                "\n\nThis can indicate insufficient privileges for this item.");
                    }

                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Uprawnienia nie zostały zmienione",
                            "Permissions did not change"),
                        mismatchText);
                    return false;
                }

                permissionWriteVerifiedThisApply = true;
            }

            const QString requestedName =
                nameEdit->text().trimmed();

            if ((workingUrl.isLocalFile()
                 || isAdminUrl(workingUrl))
                && !requestedName.isEmpty()
                && requestedName != workingName) {
                if (!validNewName(
                        requestedName)) {
                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Właściwości",
                            "Properties"),
                        trLocal(
                            "Podana nazwa jest nieprawidłowa.",
                            "The requested name is invalid."));
                    return false;
                }

                const QUrl newUrl =
                    siblingUrlWithName(
                        workingUrl,
                        requestedName);

                KIO::CopyJob *renameJob =
                    KIO::moveAs(
                        workingUrl,
                        newUrl,
                        KIO::HideProgressInfo);

                recordCopyJobForUndo(renameJob);

                if (!renameJob->exec()) {
                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Właściwości",
                            "Properties"),
                        isPolish()
                            ? QStringLiteral(
                                "Nie udało się zmienić nazwy:\n%1")
                                .arg(
                                    renameJob->errorString())
                            : QStringLiteral(
                                "Could not rename the item:\n%1")
                                .arg(
                                    renameJob->errorString()));
                    return false;
                }

                workingUrl = newUrl;
                workingName = requestedName;
                headerName->setText(
                    requestedName);
            }

            const bool recursiveApplied =
                isDir
                && recursivePermissions->isChecked();

            QString successText;
            if (recursiveApplied) {
                successText =
                    adminUnlocked
                        ? trLocal(
                            "Zastosowano zmianę rekurencyjnie przez KIO admin://. Uprawnienia folderu głównego zostały ponownie odczytane i zweryfikowane.",
                            "The recursive change was applied through KIO admin://. The top-level folder permissions were read back and verified.")
                        : trLocal(
                            "Zastosowano zmianę rekurencyjnie. Uprawnienia folderu głównego zostały ponownie odczytane i zweryfikowane.",
                            "The recursive change was applied. The top-level folder permissions were read back and verified.");
            } else {
                successText =
                    adminUnlocked
                        ? trLocal(
                            "Zapisano i zweryfikowano zmiany przez KIO admin://.",
                            "Changes were saved and verified through KIO admin://.")
                        : trLocal(
                            "Zapisano i zweryfikowano zmiany.",
                            "Changes were saved and verified.");
            }

            if (permissionWriteVerifiedThisApply
                && permissionBehaviorMayDependOnMount) {
                successText +=
                    isPolish()
                        ? QStringLiteral(
                            "\n\nZmiana została faktycznie potwierdzona po ponownym odczycie na woluminie „%1”. Dla tej konfiguracji edycja uprawnień działa i nie jest blokowana ze względu na sam typ systemu plików.")
                            .arg(
                                fileSystem.isEmpty()
                                    ? QStringLiteral("?")
                                    : fileSystem)
                        : QStringLiteral(
                            "\n\nThe change was confirmed by reading the mode back on the “%1” volume. Permission editing works with this configuration and is not blocked based on the filesystem type alone.")
                            .arg(
                                fileSystem.isEmpty()
                                    ? QStringLiteral("?")
                                    : fileSystem);
            }

            permissionsInfo->setText(successText);

            statusBar()->showMessage(
                trLocal(
                    "Zapisano zmiany we właściwościach.",
                    "Property changes saved."),
                3500);

            refreshCurrent();
            return true;
        };

        connect(
            buttons->button(
                QDialogButtonBox::Apply),
            &QPushButton::clicked,
            &dialog,
            [&] {
                applyChanges();
            });

        connect(
            buttons,
            &QDialogButtonBox::accepted,
            &dialog,
            [&] {
                if (applyChanges()) {
                    dialog.accept();
                }
            });

        connect(
            buttons,
            &QDialogButtonBox::rejected,
            &dialog,
            &QDialog::reject);

        dialog.exec();
    }

    QUrl containingDirectoryForResult(
        const QUrl &url) const
    {
        if (!url.isValid()) {
            return {};
        }

        QUrl parent = url;

        QString path = parent.path();
        while (path.size() > 1
               && path.endsWith(QLatin1Char('/'))) {
            path.chop(1);
        }

        const int slash =
            path.lastIndexOf(QLatin1Char('/'));

        if (slash < 0) {
            return {};
        }

        path =
            slash == 0
                ? QStringLiteral("/")
                : path.left(slash);

        parent.setPath(path);
        parent.setQuery(QString());
        parent.setFragment(QString());

        return parent;
    }

    void openResultLocation(
        const QUrl &url)
    {
        const QUrl parent =
            containingDirectoryForResult(url);

        if (!parent.isValid()) {
            statusBar()->showMessage(
                trLocal(
                    "Nie udało się ustalić lokalizacji elementu.",
                    "Could not determine the item's location."),
                4000);
            return;
        }

        navigateTo(parent, true);
    }

    void showDirectoryContextMenu(const QPoint &pos)
    {
        showPaneContextMenu(PaneId::Primary, false, pos);
    }

    void showDirectoryDetailsContextMenu(const QPoint &pos)
    {
        showPaneContextMenu(PaneId::Primary, true, pos);
    }

    void showPaneContextMenu(PaneId pane, bool details, const QPoint &pos)
    {
        setActivePane(pane);
        auto *list = pane == PaneId::Split ? m_splitPane->listView() : m_directoryList;
        auto *tree = pane == PaneId::Split ? m_splitPane->detailsView() : m_directoryDetails;
        QAbstractItemView *view = details ? static_cast<QAbstractItemView *>(tree) : list;
        view->setFocus(Qt::MouseFocusReason);
        PaneItem clicked;
        bool hasItem = false;
        if (details) {
            if (auto *item = tree->itemAt(pos)) {
                hasItem = true;
                if (!item->isSelected()) {
                    tree->clearSelection();
                    tree->setCurrentItem(item);
                    item->setSelected(true);
                }
                clicked = {QUrl(item->data(0, Qt::UserRole).toString()),
                    item->text(0), item->text(1), item->text(2), item->text(3),
                    item->data(0, Qt::UserRole + 1).toBool()};
            }
        } else if (auto *item = list->itemAt(pos)) {
            hasItem = true;
            if (!item->isSelected()) {
                list->clearSelection();
                list->setCurrentItem(item);
                item->setSelected(true);
            }
            clicked = {QUrl(item->data(Qt::UserRole).toString()), item->text(),
                item->data(Qt::UserRole + 2).toString(),
                item->data(Qt::UserRole + 3).toString(),
                item->data(Qt::UserRole + 4).toString(),
                item->data(Qt::UserRole + 1).toBool()};
        }
        // Snapshot before QMenu::exec(): focus and directory contents may change.
        const PaneContext context = paneContext();
        const QPoint globalPosition = view->viewport()->mapToGlobal(pos);
        {
            QScopedValueRollback<const PaneContext *> operation(m_operationContext, &context);
            showPaneMenu(context, clicked, hasItem, globalPosition);
        }
        updateFileActionStates();
    }

    void showPaneMenu(const PaneContext &context, const PaneItem &clicked,
                      bool hasItem, const QPoint &globalPosition)
    {
        if (!hasItem) {
            QMenu backgroundMenu(this);

            addViewSubmenu(backgroundMenu);
            addSortSubmenu(backgroundMenu);

            backgroundMenu.addSeparator();

            QAction *refresh =
                backgroundMenu.addAction(
                    themedIcon(QStringLiteral("view-refresh")),
                    trLocal("Odśwież", "Refresh"));

            QAction *selectAll =
                backgroundMenu.addAction(
                    themedIcon(QStringLiteral("edit-select-all")),
                    trLocal("Zaznacz wszystko", "Select all"));

            backgroundMenu.addSeparator();

            QAction *newFolder =
                backgroundMenu.addAction(
                    themedIcon(QStringLiteral("folder-new")),
                    trLocal("Nowy folder", "New folder"));
            newFolder->setEnabled(
                canModifyCurrentDirectory());

            QAction *paste =
                backgroundMenu.addAction(
                    themedIcon(QStringLiteral("edit-paste")),
                    trLocal("Wklej", "Paste"));
            paste->setEnabled(canPasteHere());

            backgroundMenu.addSeparator();

            QAction *openDolphin =
                backgroundMenu.addAction(
                    themedIcon(QStringLiteral("system-file-manager")),
                    trLocal(
                        "Otwórz w Dolphinie",
                        "Open in Dolphin"));
            openDolphin->setEnabled(
                !isSearchLocation(context.directory));

            QAction *duplicateTabAction =
                backgroundMenu.addAction(
                    themedIcon(QStringLiteral("tab-new")),
                    trLocal(
                        "Otwórz ten folder w nowej karcie",
                        "Open this folder in new tab"));
            duplicateTabAction->setEnabled(
                !isSearchLocation(context.directory));

            QAction *newWindowAction =
                backgroundMenu.addAction(
                    themedIcon(QStringLiteral("window-new")),
                    trLocal(
                        "Otwórz ten folder w nowym oknie",
                        "Open this folder in new window"));
            newWindowAction->setEnabled(
                !isSearchLocation(context.directory));

            QAction *openSplitAction =
                backgroundMenu.addAction(
                    themedIcon(QStringLiteral("view-split-left-right"), QStringLiteral("view-list-details")),
                    trLocal(
                        "Otwórz ten folder w drugim panelu",
                        "Open this folder in other pane"));
            openSplitAction->setEnabled(
                !isSearchLocation(context.directory));

            QAction *openTerminal =
                backgroundMenu.addAction(
                    themedIcon(QStringLiteral("utilities-terminal")),
                    trLocal(
                        "Otwórz terminal tutaj",
                        "Open terminal here"));
            openTerminal->setEnabled(
                context.directory.isLocalFile());

            QAction *openAdmin = nullptr;
            if (context.directory.isLocalFile()) {
                backgroundMenu.addSeparator();
                openAdmin =
                    backgroundMenu.addAction(
                        themedIcon(QStringLiteral("security-high")),
                        trLocal(
                            "Otwórz ten folder jako administrator",
                            "Open this folder as administrator"));
            }

            QAction *chosen =
                backgroundMenu.exec(
                    globalPosition);

            if (chosen == refresh) {
                refreshPane(context.id);
            } else if (chosen == selectAll) {
                selectAllDirectoryItems();
            } else if (chosen == newFolder) {
                createNewFolder();
            } else if (chosen == paste) {
                pasteClipboard();
            } else if (chosen == openDolphin) {
                openInDolphin(context.directory);
            } else if (chosen == duplicateTabAction) {
                createNewTab(context.directory, true);
            } else if (chosen == newWindowAction) {
                openInNewWindow(context.directory);
            } else if (chosen == openSplitAction) {
                openInOtherPane(context.id, context.directory);
            } else if (chosen == openTerminal) {
                openTerminalAt(context.directory);
            } else if (
                openAdmin
                && chosen == openAdmin) {
                openAsAdministrator(
                    context.directory,
                    true);
            }

            return;
        }

        const QUrl url = clicked.url;
        const bool isDir = clicked.isDir;

        const QList<QUrl> selected = selectedUrls();
        const bool single = selected.size() == 1;

        QMenu menu(this);

        QAction *openAction = menu.addAction(
            themedIcon(
                isDir
                    ? QStringLiteral("folder-open")
                    : QStringLiteral("document-open")),
            trLocal("Otwórz", "Open"));

        QAction *openNewTabAction = nullptr;
        QAction *openNewWindowAction = nullptr;
        if (single && isDir) {
            openNewTabAction = menu.addAction(
                themedIcon(QStringLiteral("tab-new")),
                trLocal(
                    "Otwórz w nowej karcie",
                    "Open in new tab"));

            openNewWindowAction = menu.addAction(
                themedIcon(QStringLiteral("window-new")),
                trLocal(
                    "Otwórz w nowym oknie",
                    "Open in new window"));
        }

        QAction *openSplitPaneAction = nullptr;
        if (single && isDir) {
            openSplitPaneAction = menu.addAction(
                themedIcon(QStringLiteral("view-split-left-right"), QStringLiteral("view-list-details")),
                trLocal(
                    "Otwórz w drugim panelu",
                    "Open in other pane"));
        }

        QAction *openDolphinAction = menu.addAction(
            themedIcon(QStringLiteral("system-file-manager")),
            trLocal(
                "Otwórz w Dolphinie",
                "Open in Dolphin"));

        QAction *openLocationAction = nullptr;
        if (isSearchLocation(context.directory) && single) {
            openLocationAction =
                menu.addAction(
                    themedIcon(QStringLiteral("folder-open")),
                    isDir
                        ? trLocal(
                            "Otwórz folder nadrzędny",
                            "Open parent folder")
                        : trLocal(
                            "Otwórz lokalizację pliku",
                            "Open file location"));
        }

        addOpenWithSubmenu(menu, selected);

        QAction *printAction = menu.addAction(
            themedIcon(QStringLiteral("document-print")),
            trLocal("Drukuj…", "Print…"));
        printAction->setEnabled(
            single && canPrintUrl(url, isDir));

        QAction *wallpaperAction = nullptr;
        if (single && isLocalImageUrl(url, isDir)) {
            wallpaperAction =
                menu.addAction(
                    themedIcon(
                        QStringLiteral(
                            "preferences-desktop-wallpaper")),
                    trLocal(
                        "Ustaw jako tło pulpitu",
                        "Set as desktop wallpaper"));
            wallpaperAction->setEnabled(
                canSetWallpaper(url, isDir));
        }

        addSendToSubmenu(
            menu,
            selected);

        QAction *openTerminalAction =
            menu.addAction(
                themedIcon(QStringLiteral("utilities-terminal")),
                trLocal(
                    "Otwórz terminal tutaj",
                    "Open terminal here"));
        openTerminalAction->setEnabled(
            isDir && url.isLocalFile());

        QAction *openAdminAction = nullptr;
        if (single && url.isLocalFile()) {
            openAdminAction =
                menu.addAction(
                    themedIcon(QStringLiteral("security-high")),
                    isDir
                        ? trLocal(
                            "Otwórz jako administrator",
                            "Open as administrator")
                        : trLocal(
                            "Otwórz lokalizację jako administrator",
                            "Open location as administrator"));
        }

        menu.addSeparator();

        QAction *cutAction = menu.addAction(
            themedIcon(QStringLiteral("edit-cut")),
            trLocal("Wytnij", "Cut"));

        QAction *copyAction = menu.addAction(
            themedIcon(QStringLiteral("edit-copy")),
            trLocal("Kopiuj", "Copy"));

        QAction *renameAction = menu.addAction(
            themedIcon(QStringLiteral("edit-rename")),
            trLocal("Zmień nazwę", "Rename"));
        renameAction->setEnabled(single);

        QAction *trashAction = menu.addAction(
            themedIcon(QStringLiteral("user-trash")),
            trLocal("Do Kosza", "Trash"));

        bool allLocal = !selected.isEmpty();
        for (const QUrl &selectedUrl : selected) {
            allLocal = allLocal && selectedUrl.isLocalFile();
        }
        trashAction->setEnabled(allLocal);

        QAction *pasteIntoAction =
            menu.addAction(
                themedIcon(QStringLiteral("edit-paste")),
                trLocal(
                    "Wklej do tego folderu",
                    "Paste into this folder"));
        pasteIntoAction->setEnabled(
            isDir
            && QApplication::clipboard()->mimeData()
            && QApplication::clipboard()->mimeData()->hasUrls());

        menu.addSeparator();

        QAction *copyAddressAction = menu.addAction(
            themedIcon(QStringLiteral("edit-copy")),
            trLocal(
                "Kopiuj adres",
                "Copy address"));

        QAction *propertiesAction =
            menu.addAction(
                themedIcon(QStringLiteral("document-properties")),
                trLocal("Właściwości", "Properties"));

        QAction *chosen =
            menu.exec(
                globalPosition);

        if (chosen == openAction) {
            if (isDir) {
                navigatePane(context.id, url);
            } else {
                QDesktopServices::openUrl(url);
            }
        } else if (
            openNewTabAction
            && chosen == openNewTabAction) {
            createNewTab(url, true);
        } else if (
            openNewWindowAction
            && chosen == openNewWindowAction) {
            openInNewWindow(url);
        } else if (
            openSplitPaneAction
            && chosen == openSplitPaneAction) {
            openInOtherPane(context.id, url);
        } else if (chosen == openDolphinAction) {
            openInDolphin(url);
        } else if (
            openLocationAction
            && chosen == openLocationAction) {
            openResultLocation(url);
        } else if (chosen == printAction) {
            printUrl(url);
        } else if (
            wallpaperAction
            && chosen == wallpaperAction) {
            setAsDesktopWallpaper(url);
        } else if (chosen == openTerminalAction) {
            openTerminalAt(url);
        } else if (
            openAdminAction
            && chosen == openAdminAction) {
            openAsAdministrator(
                url,
                isDir);
        } else if (chosen == cutAction) {
            putSelectionOnClipboard(true);
        } else if (chosen == copyAction) {
            putSelectionOnClipboard(false);
        } else if (chosen == renameAction) {
            renameSelected();
        } else if (chosen == trashAction) {
            trashSelected();
        } else if (chosen == pasteIntoAction) {
            pasteClipboardInto(url);
        } else if (chosen == copyAddressAction) {
            QGuiApplication::clipboard()->setText(
                urlForDisplay(url));
        } else if (chosen == propertiesAction) {
            showPropertiesDialog(
                clicked.name,
                url,
                isDir,
                clicked.type,
                clicked.size,
                clicked.modified);
        }

        updateFileActionStates();
    }


    QList<QUrl> selectedUrls() const
    {
        QList<QUrl> urls;
        for (const auto &item : paneContext().items) {
            if (item.url.isValid()) urls.push_back(item.url);
        }
        return urls;
    }

    bool canModifyCurrentDirectory() const
    {
        const auto context = paneContext();
        return context.isDirectory && context.directory.isValid()
            && !sameLocation(context.directory, kThisPcUrl)
            && context.directory.scheme() != QStringLiteral("trash")
            && !isSearchLocation(context.directory);
    }

    bool canPasteHere() const
    {
        if (!canModifyCurrentDirectory()) {
            return false;
        }

        const QMimeData *mime =
            QApplication::clipboard()->mimeData();

        return mime && mime->hasUrls() && !mime->urls().isEmpty();
    }

    void updateFileActionStates()
    {
        if (!m_copyAction) {
            return;
        }

        if (m_backAction && m_contentStack) updateNavigationActions();
        const bool split = paneContext().id == PaneId::Split;
        const int mode = split ? m_splitPane->viewMode() : m_directoryViewMode;
        const int sort = split ? m_splitPane->sortKey() : m_sortKey;
        if (m_viewButton && m_viewButton->menu()) {
            for (auto *action : m_viewButton->menu()->actions()) {
                if (action->data().isValid()) {
                    QSignalBlocker blocker(action);
                    action->setChecked(action->data().toInt() == mode);
                }
            }
        }
        if (m_sortButton && m_sortButton->menu()) {
            for (auto *action : m_sortButton->menu()->actions()) {
                if (action->data().isValid()) {
                    QSignalBlocker blocker(action);
                    action->setChecked(action->data().toInt() == sort);
                }
            }
        }

        const QList<QUrl> selection = selectedUrls();
        const bool hasSelection = !selection.isEmpty();
        const bool singleSelection = selection.size() == 1;

        bool allLocal = hasSelection;
        for (const QUrl &url : selection) {
            allLocal = allLocal && url.isLocalFile();
        }

        m_copyAction->setEnabled(hasSelection);
        m_cutAction->setEnabled(hasSelection);
        m_renameAction->setEnabled(singleSelection);
        if (m_propertiesAction) m_propertiesAction->setEnabled(singleSelection);
        m_trashAction->setEnabled(allLocal);

        const bool canCreate =
            canModifyCurrentDirectory();
        m_newFolderAction->setEnabled(canCreate);
        if (m_newTextFileAction) {
            m_newTextFileAction->setEnabled(canCreate);
        }
        if (m_newEmptyFileAction) {
            m_newEmptyFileAction->setEnabled(canCreate);
        }
        if (m_newButton) {
            m_newButton->setEnabled(canCreate);
        }
        m_pasteAction->setEnabled(
            canPasteHere());

        const bool inDirectory = paneContext().isDirectory;

        if (m_viewButton) {
            m_viewButton->setEnabled(inDirectory);
        }

        if (m_sortButton) {
            m_sortButton->setEnabled(inDirectory);
        }

        if (m_openDolphinAction) {
            m_openDolphinAction->setEnabled(
                !isSearchLocation(paneContext().directory));
        }
    }

    void putSelectionOnClipboard(bool cut)
    {
        const QList<QUrl> urls = selectedUrls();

        if (urls.isEmpty()) {
            return;
        }

        auto *mimeData = new QMimeData;
        mimeData->setUrls(urls);

        // This is the KDE convention understood by file managers such as
        // Dolphin. "1" means cut/move, "0" means normal copy.
        mimeData->setData(
            QStringLiteral("application/x-kde-cutselection"),
            cut ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));

        QApplication::clipboard()->setMimeData(mimeData);

        statusBar()->showMessage(
            cut
                ? (isPolish()
                    ? QStringLiteral("Wycięto do schowka: %1 elementów")
                        .arg(urls.size())
                    : QStringLiteral("Cut to clipboard: %1 items")
                        .arg(urls.size()))
                : (isPolish()
                    ? QStringLiteral("Skopiowano do schowka: %1 elementów")
                        .arg(urls.size())
                    : QStringLiteral("Copied to clipboard: %1 items")
                        .arg(urls.size())),
            4000);

        updateFileActionStates();
    }

    void pasteClipboard()
    {
        const QUrl destination = paneContext().directory;
        if (canPasteHere()) pasteClipboardInto(destination);
    }

    void createNewFile(
        const QString &suggestedName,
        const QByteArray &contents)
    {
        if (!canModifyCurrentDirectory()) {
            return;
        }

        const QUrl directory = paneContext().directory;
        bool ok = false;
        const QString initial =
            suggestedName.isEmpty()
                ? trLocal("Nowy plik", "New file")
                : suggestedName;

        const QString name =
            QInputDialog::getText(
                this,
                trLocal("Nowy plik", "New file"),
                trLocal("Nazwa pliku:", "File name:"),
                QLineEdit::Normal,
                initial,
                &ok)
                .trimmed();

        if (!ok) {
            return;
        }

        if (!validNewName(name)) {
            QMessageBox::warning(
                this,
                trLocal("Nieprawidłowa nazwa", "Invalid name"),
                trLocal(
                    "Nazwa pliku jest pusta albo zawiera niedozwolony znak „/”.",
                    "The file name is empty or contains the invalid “/” character."));
            return;
        }

        const QUrl destination =
            childUrlWithName(directory, name);

        KIO::StoredTransferJob *job =
            KIO::storedPut(
                contents,
                destination,
                -1,
                KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        recordFileJobForUndo(
            KIO::FileUndoManager::Put,
            {},
            destination,
            job);

        watchFileOperation(
            job,
            trLocal("Utworzono plik", "File created"),
            false,
            trLocal("Tworzenie pliku", "Creating file"));
    }

    void createNewFolder()
    {
        if (!canModifyCurrentDirectory()) {
            return;
        }

        const QUrl directory = paneContext().directory;
        bool ok = false;

        const QString name =
            QInputDialog::getText(
                this,
                trLocal("Nowy folder", "New folder"),
                trLocal("Nazwa folderu:", "Folder name:"),
                QLineEdit::Normal,
                trLocal("Nowy folder", "New folder"),
                &ok)
                .trimmed();

        if (!ok) {
            return;
        }

        if (!validNewName(name)) {
            QMessageBox::warning(
                this,
                trLocal("Nieprawidłowa nazwa", "Invalid name"),
                trLocal(
                    "Nazwa folderu jest pusta albo zawiera niedozwolony znak „/”.",
                    "The folder name is empty or contains the invalid “/” character."));
            return;
        }

        const QUrl destination =
            childUrlWithName(directory, name);

        KIO::MkdirJob *job =
            KIO::mkdir(destination);

        job->setUiDelegate(nullptr);
        recordFileJobForUndo(
            KIO::FileUndoManager::Mkdir,
            {},
            destination,
            job);

        watchFileOperation(
            job,
            trLocal("Utworzono folder", "Folder created"),
            false,
            trLocal("Tworzenie folderu", "Creating folder"));
    }

    void renameSelected()
    {
        const QList<QUrl> urls = selectedUrls();

        if (urls.size() != 1) {
            return;
        }

        const QUrl source = urls.first();

        const auto context = paneContext();
        QString oldName = context.items.isEmpty() ? QString() : context.items.first().name;

        if (oldName.isEmpty()) {
            oldName =
                QFileInfo(source.path()).fileName();
        }

        bool ok = false;

        const QString newName =
            QInputDialog::getText(
                this,
                trLocal("Zmień nazwę", "Rename"),
                trLocal("Nowa nazwa:", "New name:"),
                QLineEdit::Normal,
                oldName,
                &ok)
                .trimmed();

        if (!ok || newName == oldName) {
            return;
        }

        if (!validNewName(newName)) {
            QMessageBox::warning(
                this,
                trLocal("Nieprawidłowa nazwa", "Invalid name"),
                trLocal(
                    "Nazwa jest pusta albo zawiera niedozwolony znak „/”.",
                    "The name is empty or contains the invalid “/” character."));
            return;
        }

        const QUrl destination =
            siblingUrlWithName(source, newName);

        KIO::CopyJob *job =
            KIO::moveAs(
                source,
                destination,
                KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        recordCopyJobForUndo(job);

        watchFileOperation(
            job,
            trLocal("Zmieniono nazwę", "Renamed"),
            false,
            trLocal("Zmiana nazwy", "Renaming"));
    }

    void trashSelected()
    {
        const QList<QUrl> urls = selectedUrls();

        if (urls.isEmpty()) {
            return;
        }

        for (const QUrl &url : urls) {
            if (!url.isLocalFile()) {
                QMessageBox::information(
                    this,
                    trLocal("Kosz", "Trash"),
                    trLocal(
                        "W tej wersji usuwanie do Kosza jest dostępne tylko dla lokalnych plików i folderów.",
                        "In this version, moving to Trash is available only for local files and folders."));
                return;
            }
        }

        const QString question =
            isPolish()
                ? QStringLiteral(
                    "Przenieść zaznaczone elementy do Kosza?\n\nLiczba elementów: %1")
                    .arg(urls.size())
                : QStringLiteral(
                    "Move the selected items to Trash?\n\nItems: %1")
                    .arg(urls.size());

        if (QMessageBox::question(
                this,
                trLocal("Przenieś do Kosza", "Move to Trash"),
                question,
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }

        KIO::CopyJob *job =
            KIO::trash(
                urls,
                KIO::HideProgressInfo);

        job->setUiDelegate(nullptr);
        recordFileJobForUndo(
            KIO::FileUndoManager::Trash,
            urls,
            QUrl(QStringLiteral("trash:/")),
            job);

        watchFileOperation(
            job,
            trLocal(
                "Przeniesiono do Kosza",
                "Moved to Trash"),
            false,
            trLocal("Przenoszenie do Kosza", "Moving to Trash"));
    }

    void watchFileOperation(
        KJob *job,
        const QString &successMessage,
        bool clearClipboardOnSuccess = false,
        const QString &operationTitle = QString())
    {
        if (!job) {
            return;
        }

        trackFileOperation(
            job,
            operationTitle.isEmpty()
                ? trLocal("Operacja plikowa", "File operation")
                : operationTitle);

        statusBar()->showMessage(
            trLocal("Trwa operacja…", "Operation in progress…"));

        connect(
            job,
            &KJob::result,
            this,
            [this, job, successMessage, clearClipboardOnSuccess](KJob *) {
            FileOperationItem *operationItem =
                m_fileOperationByJob.value(job, nullptr);
            const bool cancelled =
                (operationItem && operationItem->cancelRequested)
                || job->error() == KJob::KilledJobError
                || job->error() == KIO::ERR_USER_CANCELED;

            const QString operationError =
                job->error() ? job->errorString() : QString();

            finishFileOperation(
                job,
                job->error() == 0,
                cancelled,
                operationError);

            if (cancelled) {
                statusBar()->showMessage(
                    trLocal("Operacja anulowana", "Operation cancelled"),
                    4000);

                refreshCurrent();
                if (m_splitPane && m_splitPane->isVisible()) {
                    m_splitPane->refresh();
                }
                updateFileActionStates();
                return;
            }

            if (job->error()) {
                statusBar()->showMessage(
                    job->errorString(),
                    7000);

                QMessageBox::warning(
                    this,
                    trLocal("Operacja nie powiodła się", "Operation failed"),
                    job->errorString());

                refreshCurrent();
                if (m_splitPane && m_splitPane->isVisible()) {
                    m_splitPane->refresh();
                }
                updateFileActionStates();
                return;
            }

            if (clearClipboardOnSuccess) {
                QApplication::clipboard()->clear();
            }

            statusBar()->showMessage(
                successMessage,
                4000);

            refreshCurrent();
            if (m_splitPane && m_splitPane->isVisible()) {
                m_splitPane->refresh();
            }
            updateFileActionStates();
        });
    }

    void setShowHiddenFiles(bool show)
    {
        if (m_showHiddenFiles == show) {
            return;
        }

        m_showHiddenFiles = show;

        if (m_showHiddenAction
            && m_showHiddenAction->isChecked() != show) {
            m_showHiddenAction->blockSignals(true);
            m_showHiddenAction->setChecked(show);
            m_showHiddenAction->blockSignals(false);
        }

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/showHidden"),
            show);

        if (m_contentStack
            && m_contentStack->currentWidget()
                == m_directoryPage) {
            loadDirectory(m_currentUrl);
        }
        if (m_splitPane) m_splitPane->setDisplayOptions(m_showHiddenFiles, m_thumbnailsEnabled);
    }

    void setThumbnailsEnabled(bool enabled)
    {
        if (m_thumbnailsEnabled == enabled) {
            return;
        }

        m_thumbnailsEnabled = enabled;

        if (m_thumbnailsAction
            && m_thumbnailsAction->isChecked() != enabled) {
            m_thumbnailsAction->blockSignals(true);
            m_thumbnailsAction->setChecked(enabled);
            m_thumbnailsAction->blockSignals(false);
        }

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/thumbnails"),
            enabled);

        if (!enabled) {
            m_thumbnailCache.clear();
        }

        if (m_contentStack
            && m_contentStack->currentWidget()
                == m_directoryPage) {
            renderDirectoryItems();
        }
        if (m_splitPane) m_splitPane->setDisplayOptions(m_showHiddenFiles, m_thumbnailsEnabled);
    }

    void setAlwaysShowFullNames(bool enabled)
    {
        m_alwaysShowFullNames = enabled;

        if (m_fullNamesAction
            && m_fullNamesAction->isChecked() != enabled) {
            QSignalBlocker blocker(m_fullNamesAction);
            m_fullNamesAction->setChecked(enabled);
        }

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/alwaysShowFullNames"),
            enabled);

        if (m_directoryList) {
            m_directoryList->setAlwaysShowFullNames(enabled);
        }
        if (m_splitPane) {
            m_splitPane->setAlwaysShowFullNames(enabled);
        }
    }

    void handleDroppedUrls(
        const QList<QUrl> &urls,
        const QUrl &destination,
        const QPoint &globalPosition,
        Qt::KeyboardModifiers modifiers)
    {
        if (urls.isEmpty() || !destination.isValid()) {
            return;
        }

        if (dropWouldCreateCycle(urls, destination)) {
            QMessageBox::warning(
                this,
                trLocal(
                    "Nieprawidłowe miejsce docelowe",
                    "Invalid destination"),
                trLocal(
                    "Nie można skopiować ani przenieść folderu do niego samego lub do jednego z jego podfolderów.",
                    "A folder cannot be copied or moved into itself or one of its subfolders."));
            return;
        }

        const bool forcedAction =
            modifiers.testFlag(Qt::ControlModifier)
            || modifiers.testFlag(Qt::ShiftModifier);

        Qt::DropAction action =
            dropActionForUrls(urls, destination, modifiers);

        if (!forcedAction) {
            QMenu menu(this);

            QAction *copyHere = menu.addAction(
                themedIcon(QStringLiteral("edit-copy")),
                trLocal("Kopiuj tutaj", "Copy here"));

            QAction *moveHere = menu.addAction(
                themedIcon(QStringLiteral("go-jump")),
                trLocal("Przenieś tutaj", "Move here"));

            menu.setDefaultAction(
                action == Qt::MoveAction
                    ? moveHere
                    : copyHere);

            menu.addSeparator();
            QAction *cancel = menu.addAction(
                trLocal("Anuluj", "Cancel"));

            QAction *chosen = menu.exec(globalPosition);
            if (!chosen || chosen == cancel) {
                return;
            }

            action = chosen == moveHere
                ? Qt::MoveAction
                : Qt::CopyAction;
        }

        KIO::CopyJob *job =
            action == Qt::MoveAction
                ? KIO::move(
                    urls,
                    destination,
                    KIO::HideProgressInfo)
                : KIO::copy(
                    urls,
                    destination,
                    KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        recordCopyJobForUndo(job);

        watchFileOperation(
            job,
            action == Qt::MoveAction
                ? trLocal("Przenoszenie zakończone", "Move completed")
                : trLocal("Kopiowanie zakończone", "Copy completed"),
            false,
            action == Qt::MoveAction
                ? trLocal("Przenoszenie", "Moving")
                : trLocal("Kopiowanie", "Copying"));
    }

    void setDirectoryViewMode(int mode)
    {
        m_directoryViewMode =
            std::clamp(mode, 0, 2);

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/viewMode"),
            m_directoryViewMode);

        applyDirectoryViewMode(false);
    }

    void applyDirectoryViewMode(bool saveSetting)
    {
        if (!m_directoryList
            || !m_directoryDetails
            || !m_directoryViewStack) {
            return;
        }

        if (saveSetting) {
            QSettings settings;
            settings.setValue(
                QStringLiteral("directory/viewMode"),
                m_directoryViewMode);
        }

        m_directoryList->clearSelection();
        m_directoryDetails->clearSelection();

        if (m_directoryViewMode == 0) {
            m_directoryList->setViewMode(
                QListView::IconMode);
            m_directoryList->setFlow(
                QListView::LeftToRight);
            m_directoryList->setWrapping(true);
            m_directoryList->setIconSize(
                QSize(64, 64));
            m_directoryList->setGridSize(QSize());
            m_directoryList->setSpacing(3);
            m_directoryList->setUniformItemSizes(false);
            m_directoryViewStack->setCurrentWidget(
                m_directoryList);

            if (m_viewButton) {
                m_viewButton->setIcon(
                    themedIcon(
                        QStringLiteral("view-list-icons")));
            }
        } else if (m_directoryViewMode == 1) {
            m_directoryList->setViewMode(
                QListView::ListMode);
            m_directoryList->setFlow(
                QListView::TopToBottom);
            m_directoryList->setWrapping(false);
            m_directoryList->setIconSize(
                QSize(24, 24));
            m_directoryList->setGridSize(QSize());
            m_directoryList->setSpacing(1);
            m_directoryList->setUniformItemSizes(false);
            m_directoryViewStack->setCurrentWidget(
                m_directoryList);

            if (m_viewButton) {
                m_viewButton->setIcon(
                    themedIcon(
                        QStringLiteral("view-list-text")));
            }
        } else {
            m_directoryViewStack->setCurrentWidget(
                m_directoryDetails);

            if (m_viewButton) {
                m_viewButton->setIcon(
                    themedIcon(
                        QStringLiteral("view-list-details")));
            }
        }

        configureDirectoryDragDrop(m_directoryList);
        configureDirectoryDragDrop(m_directoryDetails);

        updateFileActionStates();
    }

    void setSortKey(int key)
    {
        m_sortKey = std::clamp(key, 0, 3);

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/sortKey"),
            m_sortKey);

        if (m_contentStack
            && m_contentStack->currentWidget()
                == m_directoryPage) {
            renderDirectoryItems();
        }
    }

    void setSortAscending(bool ascending)
    {
        m_sortAscending = ascending;

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/sortAscending"),
            m_sortAscending);

        if (m_sortButton) {
            m_sortButton->setIcon(
                themedIcon(
                    m_sortAscending
                        ? QStringLiteral("view-sort-ascending")
                        : QStringLiteral("view-sort-descending")));
        }

        if (m_contentStack
            && m_contentStack->currentWidget()
                == m_directoryPage) {
            renderDirectoryItems();
        }
    }

    QString displayNameForLocation(const QUrl &url) const
    {
        if (sameLocation(url, kThisPcUrl)) {
            return trLocal("Ten komputer", "This PC");
        }

        if (isSearchLocation(url)) {
            const QString query =
                searchQueryFromUrl(url);

            return query.isEmpty()
                ? trLocal(
                    "Wyniki wyszukiwania",
                    "Search results")
                : (isPolish()
                    ? QStringLiteral("Wyniki dla: %1").arg(query)
                    : QStringLiteral("Results for: %1").arg(query));
        }

        for (const DriveInfo &drive : m_drives) {
            if (sameLocation(drive.targetUrl, url)) {
                return drive.name;
            }
        }

        if (url.isLocalFile()) {
            const QString path = url.toLocalFile();

            if (path == QDir::homePath()) {
                return trLocal("Katalog domowy", "Home");
            }

            const QString fileName =
                QFileInfo(path).fileName();

            return fileName.isEmpty()
                ? path
                : fileName;
        }

        if (url.scheme() == QStringLiteral("trash")) {
            return trLocal("Kosz", "Trash");
        }

        if (url.scheme() == QStringLiteral("remote")) {
            return trLocal("Sieć", "Network");
        }

        const QString last =
            QFileInfo(url.path()).fileName();

        return last.isEmpty()
            ? url.toDisplayString()
            : last;
    }

    void rebuildBreadcrumbs()
    {
        clearLayout(m_breadcrumbLayout);

        auto addCrumb =
            [this](const QString &text,
                   const QIcon &icon,
                   const QUrl &url) {
            auto *button =
                new QToolButton(m_breadcrumbFrame);

            button->setObjectName(
                QStringLiteral("crumbButton"));
            button->setText(text);
            button->setIcon(icon);
            button->setToolButtonStyle(
                icon.isNull()
                    ? Qt::ToolButtonTextOnly
                    : Qt::ToolButtonTextBesideIcon);

            connect(
                button,
                &QToolButton::clicked,
                this,
                [this, url] {
                    if (sameLocation(url, m_currentUrl)) {
                        beginAddressEdit();
                    } else {
                        navigateTo(url, true);
                    }
                });

            m_breadcrumbLayout->addWidget(button);
        };

        auto addSeparator =
            [this] {
            auto *sep =
                new QLabel(QStringLiteral("›"), m_breadcrumbFrame);
            sep->setForegroundRole(
                QPalette::PlaceholderText);
            m_breadcrumbLayout->addWidget(sep);
        };

        if (sameLocation(m_currentUrl, kThisPcUrl)) {
            addCrumb(
                trLocal("Ten komputer", "This PC"),
                themedIcon(QStringLiteral("computer")),
                kThisPcUrl);

            m_breadcrumbLayout->addStretch(1);
            return;
        }

        QString baseName;
        QUrl baseUrl;
        QString relativePath;

        for (const DriveInfo &drive : m_drives) {
            if (!drive.targetUrl.isLocalFile()
                || !m_currentUrl.isLocalFile()) {
                continue;
            }

            const QString drivePath =
                QDir::cleanPath(
                    drive.targetUrl.toLocalFile());

            const QString currentPath =
                QDir::cleanPath(
                    m_currentUrl.toLocalFile());

            if (currentPath == drivePath
                || currentPath.startsWith(
                    drivePath + QDir::separator())) {
                baseName = drive.name;
                baseUrl = drive.targetUrl;
                relativePath =
                    QDir(drivePath).relativeFilePath(
                        currentPath);
                break;
            }
        }

        if (m_currentUrl.isLocalFile()) {
            const QString currentPath =
                QDir::cleanPath(
                    m_currentUrl.toLocalFile());

            const QString homePath =
                QDir::cleanPath(QDir::homePath());

            if (baseUrl.isEmpty()
                && (currentPath == homePath
                    || currentPath.startsWith(
                        homePath + QDir::separator()))) {
                baseName =
                    trLocal("Katalog domowy", "Home");
                baseUrl =
                    QUrl::fromLocalFile(homePath);
                relativePath =
                    QDir(homePath).relativeFilePath(
                        currentPath);
            }

            if (baseUrl.isEmpty()) {
                baseName = QStringLiteral("/");
                baseUrl = QUrl::fromLocalFile(
                    QStringLiteral("/"));
                relativePath =
                    QDir(QStringLiteral("/"))
                        .relativeFilePath(currentPath);
            }

            bool baseIsDrive = false;
            for (const DriveInfo &drive : m_drives) {
                if (sameLocation(drive.targetUrl, baseUrl)) {
                    baseIsDrive = true;
                    break;
                }
            }

            if (baseIsDrive) {
                addCrumb(
                    trLocal("Ten komputer", "This PC"),
                    themedIcon(QStringLiteral("computer")),
                    kThisPcUrl);
                addSeparator();
            }

            addCrumb(
                baseName,
                themedIcon(
                    baseName == QStringLiteral("/")
                        ? QStringLiteral("folder-root")
                        : (baseIsDrive
                            ? QStringLiteral("drive-harddisk")
                            : QStringLiteral("folder"))),
                baseUrl);

            if (relativePath != QStringLiteral(".")
                && !relativePath.isEmpty()) {
                QString cumulative =
                    baseUrl.toLocalFile();

                const QStringList parts =
                    relativePath.split(
                        QDir::separator(),
                        Qt::SkipEmptyParts);

                for (const QString &part : parts) {
                    addSeparator();
                    cumulative =
                        QDir(cumulative).filePath(part);

                    addCrumb(
                        part,
                        QIcon(),
                        QUrl::fromLocalFile(cumulative));
                }
            }
        } else {
            QString rootLabel =
                m_currentUrl.scheme();

            if (isSearchLocation(m_currentUrl)) {
                addCrumb(
                    trLocal(
                        "Ten komputer",
                        "This PC"),
                    themedIcon(QStringLiteral("computer")),
                    kThisPcUrl);
                addSeparator();
                addCrumb(
                    displayNameForLocation(m_currentUrl),
                    themedIcon(QStringLiteral("edit-find")),
                    m_currentUrl);
                m_breadcrumbLayout->addStretch(1);
                return;
            }

            if (m_currentUrl.scheme()
                == QStringLiteral("trash")) {
                rootLabel = trLocal("Kosz", "Trash");
            } else if (
                m_currentUrl.scheme()
                == QStringLiteral("remote")) {
                rootLabel = trLocal("Sieć", "Network");
            }

            QUrl rootUrl = m_currentUrl;
            rootUrl.setPath(QStringLiteral("/"));

            addCrumb(
                rootLabel,
                themedIcon(QStringLiteral("folder")),
                rootUrl);

            QString cumulativePath;

            const QStringList parts =
                m_currentUrl.path().split(
                    QLatin1Char('/'),
                    Qt::SkipEmptyParts);

            for (const QString &part : parts) {
                addSeparator();

                cumulativePath +=
                    QLatin1Char('/') + part;

                QUrl crumbUrl = m_currentUrl;
                crumbUrl.setPath(cumulativePath);

                addCrumb(
                    part,
                    QIcon(),
                    crumbUrl);
            }
        }

        m_breadcrumbLayout->addStretch(1);
    }

    QUrl parentUrl() const
    {
        if (sameLocation(m_currentUrl, kThisPcUrl)) {
            return {};
        }

        if (isSearchLocation(m_currentUrl)) {
            const QUrl base =
                searchBaseFromUrl(m_currentUrl);

            return base.isValid()
                ? base
                : kThisPcUrl;
        }

        for (const DriveInfo &drive : m_drives) {
            if (sameLocation(
                    drive.targetUrl,
                    m_currentUrl)) {
                return kThisPcUrl;
            }
        }

        QUrl parent = m_currentUrl;
        QString path = parent.path();

        if (path.isEmpty()
            || path == QStringLiteral("/")) {
            return kThisPcUrl;
        }

        while (path.size() > 1
               && path.endsWith(
                   QLatin1Char('/'))) {
            path.chop(1);
        }

        const int slash =
            path.lastIndexOf(QLatin1Char('/'));

        if (slash <= 0) {
            path = QStringLiteral("/");
        } else {
            path = path.left(slash);
        }

        parent.setPath(path);
        return parent;
    }

    void updateNavigationActions()
    {
        const bool split = paneContext().id == PaneId::Split;
        m_backAction->setEnabled(split ? m_splitPane->canGoBack() : m_historyIndex > 0);
        m_forwardAction->setEnabled(split ? m_splitPane->canGoForward()
            : m_historyIndex >= 0 && m_historyIndex < m_history.size() - 1);
        m_upAction->setEnabled(split ? m_splitPane->canGoUp()
            : !sameLocation(m_currentUrl, kThisPcUrl));
    }

    void updateSidebarCurrent()
    {
        SidebarButton *bestStatic = nullptr;
        SidebarDriveButton *bestDrive = nullptr;
        int bestDepth = -1;

        QUrl effectiveLocation =
            m_currentUrl;

        if (isSearchLocation(m_currentUrl)) {
            if (m_searchScopeMode == 2) {
                effectiveLocation =
                    kThisPcUrl;
            } else {
                const QUrl base =
                    searchBaseFromUrl(m_currentUrl);

                if (base.isValid()) {
                    effectiveLocation = base;
                }
            }
        }

        for (SidebarButton *button :
             std::as_const(m_staticSidebarButtons)) {
            button->setCurrent(false);

            if (!button->url().isValid()) {
                continue;
            }

            if (isWithinLocation(effectiveLocation, button->url())) {
                const int depth = locationDepth(button->url());
                if (depth > bestDepth) {
                    bestDepth = depth;
                    bestStatic = button;
                    bestDrive = nullptr;
                }
            }
        }

        for (SidebarDriveButton *button :
             std::as_const(m_driveSidebarButtons)) {
            button->setCurrent(false);

            if (isWithinLocation(effectiveLocation, button->url())) {
                const int depth = locationDepth(button->url());
                if (depth > bestDepth) {
                    bestDepth = depth;
                    bestDrive = button;
                    bestStatic = nullptr;
                }
            }
        }

        if (sameLocation(effectiveLocation, kThisPcUrl)) {
            if (m_thisPcButton) {
                m_thisPcButton->setCurrent(true);
            }
            return;
        }

        if (bestStatic) {
            bestStatic->setCurrent(true);
        } else if (bestDrive) {
            bestDrive->setCurrent(true);
        }
    }

private Q_SLOTS:
    void goBack()
    {
        if (m_historyIndex <= 0) {
            return;
        }

        --m_historyIndex;
        loadLocation(
            m_history.at(m_historyIndex));
    }

    void goForward()
    {
        if (m_historyIndex < 0
            || m_historyIndex >=
                m_history.size() - 1) {
            return;
        }

        ++m_historyIndex;
        loadLocation(
            m_history.at(m_historyIndex));
    }

    void goUp()
    {
        const QUrl parent = parentUrl();

        if (parent.isValid()) {
            navigateTo(parent, true);
        }
    }

    void refreshCurrent()
    {
        reloadDrives();

        if (sameLocation(
                m_currentUrl,
                kThisPcUrl)) {
            return;
        }

        if (isSearchLocation(m_currentUrl)) {
            loadSearchLocation(m_currentUrl);
        } else {
            loadDirectory(m_currentUrl);
        }
    }

    void reloadDrives()
    {
        if (m_driveJob) {
            return;
        }

        m_pendingDrives.clear();

        if (sameLocation(
                m_currentUrl,
                kThisPcUrl)) {
            m_homeStatus->setText(
                trLocal(
                    "Odświeżanie…",
                    "Refreshing…"));
        }

        KIO::ListJob *job = KIO::listDir(
            kThisPcUrl,
            KIO::HideProgressInfo);

        job->setUiDelegate(nullptr);
        m_driveJob = job;

        connect(
            job,
            &KIO::ListJob::entries,
            this,
            [this](
                KIO::Job *,
                const KIO::UDSEntryList &entries) {
            for (const KIO::UDSEntry &entry : entries) {
                const QString name =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_DISPLAY_NAME);

                if (name.isEmpty()
                    || name == QStringLiteral(".")) {
                    continue;
                }

                const QString target =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_TARGET_URL);

                if (target.isEmpty()) {
                    continue;
                }

                DriveInfo drive;
                drive.name = name;
                drive.targetUrl = QUrl(target);
                drive.iconName =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_ICON_NAME);
                drive.freeText =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_EXTRA + 0);
                drive.capacityText =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_EXTRA + 1);
                drive.usedText =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_EXTRA + 2);
                drive.fileSystem =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_EXTRA + 3);
                drive.mountPoint =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_EXTRA + 4);

                QString percentText =
                    drive.usedText;
                percentText.remove(
                    QLatin1Char('%'));

                bool ok = false;
                const int percent =
                    percentText.toInt(&ok);

                drive.usedPercent =
                    ok
                        ? std::clamp(percent, 0, 100)
                        : 0;

                m_pendingDrives.push_back(drive);
            }
        });

        connect(
            job,
            &KJob::result,
            this,
            [this, job](KJob *) {
            if (m_driveJob == job) {
                m_driveJob = nullptr;
            }

            if (job->error()) {
                if (sameLocation(
                        m_currentUrl,
                        kThisPcUrl)) {
                    m_homeStatus->setText(
                        trLocal(
                            "Nie udało się odczytać thispc:/.",
                            "Could not read thispc:/."));
                }

                return;
            }

            m_drives = m_pendingDrives;
            rebuildDriveGrid();
            rebuildSidebarDevices();
            rebuildBreadcrumbs();
            updateSidebarCurrent();
        });
    }

private:
    void rebuildDriveGrid()
    {
        clearLayout(m_drivesGrid);

        if (m_drives.isEmpty()) {
            m_homeStatus->setText(
                trLocal(
                    "Nie znaleziono dysków.",
                    "No drives found."));
            return;
        }

        m_homeStatus->clear();

        for (int i = 0;
             i < m_drives.size();
             ++i) {
            DriveFrame *card =
                makeDriveCard(
                    m_drives.at(i),
                    m_homePage);

            connect(
                card,
                &ClickableFrame::activated,
                this,
                [this](const QUrl &url) {
                    navigateTo(url, true);
                });

            m_drivesGrid->addWidget(
                card,
                i / 2,
                i % 2);
        }
    }

    void rebuildSidebarDevices()
    {
        clearLayout(m_devicesLayout);
        m_driveSidebarButtons.clear();

        for (const DriveInfo &drive :
             std::as_const(m_drives)) {
            auto *button =
                new SidebarDriveButton(
                    drive,
                    m_sidebar);

            connect(
                button,
                &SidebarDriveButton::activated,
                this,
                [this](const QUrl &url) {
                    navigateTo(url, true);
                });

            connect(
                button,
                &SidebarDriveButton::openInNewTabRequested,
                this,
                [this](const QUrl &url, bool makeCurrent) {
                    createNewTab(url, makeCurrent);
                });

            connect(
                button,
                &SidebarDriveButton::openInNewWindowRequested,
                this,
                &ThisPcWindow::openInNewWindow);
            connect(
                button,
                &SidebarDriveButton::openInSplitPaneRequested,
                this,
                &ThisPcWindow::openInSplitPane);

            m_devicesLayout->addWidget(button);
            m_driveSidebarButtons.push_back(button);
        }
    }

private:
    QAction *m_backAction = nullptr;
    QAction *m_forwardAction = nullptr;
    QAction *m_upAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_openDolphinAction = nullptr;

    QToolButton *m_newButton = nullptr;
    QAction *m_newFolderAction = nullptr;
    QAction *m_newTextFileAction = nullptr;
    QAction *m_newEmptyFileAction = nullptr;
    QAction *m_cutAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_renameAction = nullptr;
    QAction *m_propertiesAction = nullptr;
    QAction *m_trashAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    KIO::FileUndoManager *m_fileUndoManager = nullptr;
    bool m_undoRedoBusy = false;
    int m_undoRedoMode = 0;

    QToolButton *m_viewButton = nullptr;
    QToolButton *m_sortButton = nullptr;
    QAction *m_splitViewAction = nullptr;
    QAction *m_showHiddenAction = nullptr;
    QAction *m_thumbnailsAction = nullptr;
    QAction *m_fullNamesAction = nullptr;
    QAction *m_restoreSessionAction = nullptr;
    int m_directoryViewMode = 0;
    int m_sortKey = 0;
    bool m_sortAscending = true;
    bool m_showHiddenFiles = false;
    bool m_thumbnailsEnabled = true;
    bool m_alwaysShowFullNames = false;
    bool m_restorePreviousSession = true;

    QStackedWidget *m_addressStack = nullptr;
    BreadcrumbFrame *m_breadcrumbFrame = nullptr;
    QHBoxLayout *m_breadcrumbLayout = nullptr;
    AddressLineEdit *m_addressEdit = nullptr;
    QAction *m_searchScopeAction = nullptr;
    QMenu *m_searchScopeMenu = nullptr;
    QToolButton *m_searchFilterButton = nullptr;
    QActionGroup *m_searchScopeGroup = nullptr;
    QActionGroup *m_searchTypeGroup = nullptr;
    QActionGroup *m_searchDateGroup = nullptr;
    QActionGroup *m_searchSizeGroup = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QAction *m_stopSearchAction = nullptr;
    QString m_searchQuery;
    int m_searchScopeMode = 2;
    int m_searchTypeFilter = 0;
    int m_searchDateFilter = 0;
    int m_searchSizeFilter = 0;

    QFrame *m_sidebar = nullptr;
    QVBoxLayout *m_sidebarLayout = nullptr;
    QVBoxLayout *m_devicesLayout = nullptr;
    QList<SidebarButton *> m_staticSidebarButtons;
    QList<SidebarDriveButton *> m_driveSidebarButtons;
    SidebarButton *m_thisPcButton = nullptr;

    QFrame *m_tabStrip = nullptr;
    ExplorerTabBar *m_tabBar = nullptr;
    QToolButton *m_newTabButton = nullptr;
    QList<TabState> m_tabs;
    QList<TabState> m_closedTabs;
    int m_activeTab = -1;
    bool m_tabChangeInProgress = false;
    bool m_tabRestoreInProgress = false;

    QSplitter *m_contentSplitter = nullptr;
    QWidget *m_primaryPane = nullptr;
    SplitBrowserPane *m_splitPane = nullptr;
    QStackedWidget *m_contentStack = nullptr;

    QToolButton *m_operationButton = nullptr;
    QLabel *m_operationBadgeLabel = nullptr;
    QFrame *m_operationPopup = nullptr;
    QLabel *m_versionLabel = nullptr;
    QLabel *m_operationSummaryLabel = nullptr;
    QProgressBar *m_operationOverallProgress = nullptr;
    QLabel *m_operationEmptyLabel = nullptr;
    QScrollArea *m_operationScrollArea = nullptr;
    QFrame *m_operationFooter = nullptr;
    QToolButton *m_cancelAllOperationsButton = nullptr;
    QToolButton *m_clearFinishedOperationsButton = nullptr;
    QWidget *m_operationListWidget = nullptr;
    QVBoxLayout *m_operationListLayout = nullptr;
    QList<FileOperationItem *> m_fileOperationItems;
    QHash<KJob *, FileOperationItem *> m_fileOperationByJob;

    QWidget *m_homePage = nullptr;
    QLabel *m_homeStatus = nullptr;
    QGridLayout *m_drivesGrid = nullptr;

    QWidget *m_directoryPage = nullptr;
    QFrame *m_adminBanner = nullptr;
    QLabel *m_directoryTitle = nullptr;
    QLabel *m_directoryStatus = nullptr;
    QFrame *m_searchProgressFrame = nullptr;
    QProgressBar *m_searchProgressBar = nullptr;
    QPushButton *m_cancelSearchButton = nullptr;
    QStackedWidget *m_directoryViewStack = nullptr;
    DirectoryListWidget *m_directoryList = nullptr;
    DirectoryTreeWidget *m_directoryDetails = nullptr;

    QUrl m_currentUrl = kThisPcUrl;
    QList<QUrl> m_history;
    int m_historyIndex = -1;

    QList<DriveInfo> m_drives;
    QList<DriveInfo> m_pendingDrives;
    QList<FileInfo> m_pendingFiles;
    QHash<QString, QIcon> m_thumbnailCache;

    QTimer m_refreshTimer;
    QTimer m_searchRenderTimer;
    QPointer<KIO::ListJob> m_driveJob;
    QPointer<KIO::ListJob> m_directoryJob;

    QList<QPointer<KIO::ListJob>> m_searchJobs;
    QHash<KJob *, int> m_searchProgressValues;
    QSet<QString> m_searchSeenUrls;
    quint64 m_searchGeneration = 0;
    int m_searchTotalRoots = 0;
    int m_searchCompletedRoots = 0;
    int m_searchErrors = 0;
    int m_searchVisibleCount = 0;
    bool m_searchInProgress = false;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(
        QStringLiteral("kio-thispc"));
    QCoreApplication::setOrganizationDomain(
        QStringLiteral("local"));
    QCoreApplication::setApplicationName(
        QStringLiteral("thispc-view"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral("0.19.0.3"));

    app.setApplicationDisplayName(
        isPolish()
            ? QStringLiteral("Ten komputer")
            : QStringLiteral("This PC"));

    app.setWindowIcon(
        QIcon::fromTheme(
            QStringLiteral("computer")));

    app.setStyleSheet(
        applicationStyleSheet());

    QUrl initialUrl = kThisPcUrl;
    bool hasExplicitInitialUrl = false;

    if (argc > 1) {
        const QString argument =
            QString::fromLocal8Bit(argv[1]).trimmed();

        if (!argument.isEmpty()) {
            hasExplicitInitialUrl = true;
            const QUrl explicitUrl(argument);

            if (explicitUrl.isValid()
                && !explicitUrl.scheme().isEmpty()) {
                initialUrl = explicitUrl;
            } else {
                initialUrl = QUrl::fromUserInput(
                    argument,
                    QDir::currentPath(),
                    QUrl::AssumeLocalFile);
            }
        }
    }

    ThisPcWindow window(
        initialUrl,
        !hasExplicitInitialUrl);
    window.show();

    return app.exec();
}

#include "thispcview.moc"
