#include "appwidgets.h"

#include "pathwidgets.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QFocusEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QProcess>
#include <QProgressBar>
#include <QScrollBar>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{

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

void makePassive(QWidget *widget)
{
    widget->setAttribute(Qt::WA_TransparentForMouseEvents);
}

QString driveTooltip(const DriveInfo &drive)
{
    if (!drive.isMounted) {
        QString tip = drive.name + QStringLiteral("\n") + trLocal("Niezamontowany", "Unmounted");
        if (!drive.capacityText.isEmpty() && drive.capacityText != QStringLiteral("—")) {
            tip += QStringLiteral(" • ") + drive.capacityText;
        }
        if (!drive.fileSystem.isEmpty()) {
            tip += QStringLiteral("\n") + trLocal("System plików: ", "Filesystem: ") + drive.fileSystem;
        }
        return tip;
    }

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

} // namespace

void populateDriveContextMenu(QMenu &menu,
                              const DriveInfo &drive,
                              bool canSafelyRemove,
                              bool canEject,
                              QAction **outUnmountAction,
                              QAction **outSafelyRemoveAction,
                              QAction **outEjectAction,
                              QAction **outOpenAction,
                              QAction **outCopyPathAction)
{
    QAction *openAction = nullptr;
    QAction *copyPathAction = nullptr;
    QAction *unmountAction = nullptr;
    QAction *safelyRemoveAction = nullptr;
    QAction *ejectAction = nullptr;

    if (drive.isMounted) {
        openAction = menu.addAction(
            themedIcon(QStringLiteral("system-file-manager")),
            trLocal("Otwórz w Dolphinie", "Open in Dolphin"));

        copyPathAction = menu.addAction(
            themedIcon(QStringLiteral("edit-copy")),
            trLocal("Kopiuj punkt montowania", "Copy mount point"));
    }

    if (drive.isRemovable) {
        if (drive.isMounted) {
            menu.addSeparator();
            unmountAction = menu.addAction(
                themedIcon(QStringLiteral("media-eject")),
                trLocal("Odmontuj", "Unmount"));
        }

        if (canSafelyRemove) {
            safelyRemoveAction = menu.addAction(
                themedIcon(QStringLiteral("drive-removable-media")),
                trLocal("Bezpiecznie usuń", "Safely remove"));
        }

        if (canEject) {
            ejectAction = menu.addAction(
                themedIcon(QStringLiteral("media-eject")),
                trLocal("Wysuń", "Eject"));
        }
    }

    if (outOpenAction) *outOpenAction = openAction;
    if (outCopyPathAction) *outCopyPathAction = copyPathAction;
    if (outUnmountAction) *outUnmountAction = unmountAction;
    if (outSafelyRemoveAction) *outSafelyRemoveAction = safelyRemoveAction;
    if (outEjectAction) *outEjectAction = ejectAction;
}

HomePageWidget::HomePageWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
}

void HomePageWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        for (QWidget *hit = childAt(event->position().toPoint());
             hit && hit != this; hit = hit->parentWidget()) {
            if (qobject_cast<ClickableFrame *>(hit)) {
                QWidget::mousePressEvent(event);
                return;
            }
        }
        setFocus(Qt::MouseFocusReason);
        Q_EMIT backgroundClicked();
    }
    QWidget::mousePressEvent(event);
}

AddressLineEdit::AddressLineEdit(QWidget *parent)
    : QLineEdit(parent)
{
}

void AddressLineEdit::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        Q_EMIT canceled();
        event->accept();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void AddressLineEdit::focusOutEvent(QFocusEvent *event)
{
    QLineEdit::focusOutEvent(event);
    Q_EMIT focusLeft();
}

AddressBarFrame::AddressBarFrame(QWidget *parent)
    : QFrame(parent)
{
}

void AddressBarFrame::addBlankClickTarget(QWidget *target)
{
    target->installEventFilter(this);
}

bool AddressBarFrame::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            Q_EMIT blankClicked();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void AddressBarFrame::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        Q_EMIT blankClicked();
    }
    QFrame::mousePressEvent(event);
}

BreadcrumbFrame::BreadcrumbFrame(QWidget *parent)
    : AddressBarFrame(parent)
{
    connect(this, &AddressBarFrame::blankClicked,
            this, &BreadcrumbFrame::clicked);
    setCursor(Qt::IBeamCursor);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(4, 2, 4, 2);
    row->setSpacing(1);
    auto *previous = new QToolButton(this);
    previous->setArrowType(Qt::LeftArrow);
    previous->setAutoRaise(true);
    previous->setToolTip(trLocal("Przewiń ścieżkę w lewo", "Scroll path left"));
    row->addWidget(previous);
    m_scroll = new PathScrollArea(this);
    m_scroll->setWidget(new QWidget);
    row->addWidget(m_scroll, 1);
    auto *next = new QToolButton(this);
    next->setArrowType(Qt::RightArrow);
    next->setAutoRaise(true);
    next->setToolTip(trLocal("Przewiń ścieżkę w prawo", "Scroll path right"));
    row->addWidget(next);
    auto *bar = m_scroll->horizontalScrollBar();
    connect(previous, &QToolButton::clicked, this, [this, bar] {
        bar->setValue(bar->value() - qMax(1, m_scroll->viewport()->width() / 2));
    });
    connect(next, &QToolButton::clicked, this, [this, bar] {
        bar->setValue(bar->value() + qMax(1, m_scroll->viewport()->width() / 2));
    });
    connect(bar, &QScrollBar::rangeChanged, this, [previous, next, bar](int, int maximum) {
        previous->setVisible(maximum > 0);
        next->setVisible(maximum > 0);
        bar->setValue(maximum); // Keep the current folder visible after navigation/resize.
    });
    connect(bar, &QScrollBar::valueChanged, this, [previous, next, bar](int value) {
        previous->setEnabled(value > 0);
        next->setEnabled(value < bar->maximum());
    });
    previous->hide();
    next->hide();
}

QWidget *BreadcrumbFrame::contentsWidget() const
{
    return m_scroll->widget();
}

void BreadcrumbFrame::mousePressEvent(QMouseEvent *event)
{
    AddressBarFrame::mousePressEvent(event);
}

ClickableFrame::ClickableFrame(const QUrl &url, QWidget *parent)
    : QFrame(parent)
    , m_url(url)
{
    setObjectName(QStringLiteral("thispcCard"));
    setFrameShape(QFrame::NoFrame);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setProperty("navigationUrl", m_url.toString(QUrl::FullyEncoded));
}

void ClickableFrame::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void ClickableFrame::focusInEvent(QFocusEvent *event)
{
    QFrame::focusInEvent(event);
    Q_EMIT focused(m_url, event->reason());
}

void ClickableFrame::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        Q_EMIT activated(m_url);
        event->accept();
        return;
    }
    QFrame::mouseDoubleClickEvent(event);
}

void ClickableFrame::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        Q_EMIT activated(m_url);
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

QUrl ClickableFrame::targetUrl() const
{
    return m_url;
}

DriveFrame::DriveFrame(const DriveInfo &drive,
                       bool canSafelyRemove,
                       bool canEject,
                       QWidget *parent)
    : ClickableFrame(drive.targetUrl, parent)
    , m_drive(drive)
    , m_canSafelyRemove(canSafelyRemove)
    , m_canEject(canEject)
{
    setProperty("driveId", drive.id);
    if (!drive.isMounted) {
        setProperty("navigationUrl", drive.id);
    }
}

void DriveFrame::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *unmountAction = nullptr;
    QAction *safelyRemoveAction = nullptr;
    QAction *ejectAction = nullptr;
    QAction *openAction = nullptr;
    QAction *copyPathAction = nullptr;

    populateDriveContextMenu(menu, m_drive, m_canSafelyRemove, m_canEject,
                             &unmountAction, &safelyRemoveAction, &ejectAction,
                             &openAction, &copyPathAction);

    if (menu.actions().isEmpty()) {
        event->accept();
        return;
    }

    QAction *chosen = menu.exec(event->globalPos());
    if (chosen) {
        if (chosen == unmountAction) {
            Q_EMIT unmountRequested(m_drive);
        } else if (chosen == safelyRemoveAction) {
            Q_EMIT safelyRemoveRequested(m_drive);
        } else if (chosen == ejectAction) {
            Q_EMIT ejectRequested(m_drive);
        } else if (chosen == openAction) {
            openInDolphin(m_drive.targetUrl);
        } else if (chosen == copyPathAction) {
            QGuiApplication::clipboard()->setText(m_drive.mountPoint);
        }
    }
    event->accept();
}

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

DriveFrame *makeDriveCard(const DriveInfo &drive,
                          bool canSafelyRemove,
                          bool canEject,
                          QWidget *parent)
{
    auto *card = new DriveFrame(drive, canSafelyRemove, canEject, parent);
    card->setMinimumHeight(88);
    card->setMaximumHeight(94);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    card->setAccessibleName(drive.name);
    card->setToolTip(driveTooltip(drive));

    auto *outer = new QHBoxLayout(card);
    outer->setContentsMargins(13, 9, 13, 9);
    outer->setSpacing(12);

    auto *icon = new QLabel(card);
    icon->setObjectName(QStringLiteral("driveCardIcon"));

    QString themeIcon = QStringLiteral("drive-harddisk");
    if (!drive.isMounted || drive.iconName.contains(QStringLiteral("removable"))) {
        themeIcon = QStringLiteral("drive-removable-media");
    }

    icon->setPixmap(
        themedIcon(themeIcon, QStringLiteral("drive-harddisk")).pixmap(44, 44));
    icon->setFixedSize(48, 48);
    icon->setAlignment(Qt::AlignCenter);
    makePassive(icon);

    auto *bodyWidget = new QWidget(card);
    bodyWidget->setObjectName(QStringLiteral("driveCardContent"));
    bodyWidget->setMaximumWidth(335);
    bodyWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *body = new QVBoxLayout(bodyWidget);
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

    QString capacity;
    if (drive.isMounted) {
        capacity = drive.freeText
            + trLocal(" wolne z ", " free of ")
            + drive.capacityText;
    } else {
        progress->setVisible(false);
        if (!drive.capacityText.isEmpty() && drive.capacityText != QStringLiteral("—")) {
            capacity = trLocal("Niezamontowany • ", "Unmounted • ") + drive.capacityText;
        } else {
            capacity = trLocal("Niezamontowany", "Unmounted");
        }
    }

    auto *subtitle = new QLabel(capacity, card);
    subtitle->setForegroundRole(QPalette::PlaceholderText);
    makePassive(subtitle);

    body->addWidget(title);
    body->addWidget(progress);
    body->addWidget(subtitle);

    outer->addWidget(icon, 0, Qt::AlignVCenter);
    outer->addWidget(bodyWidget, 1);
    outer->addStretch(1);

    return card;
}

DriveFrame *makeDriveCard(const DriveInfo &drive, QWidget *parent)
{
    return makeDriveCard(drive, false, false, parent);
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
