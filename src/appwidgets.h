/*
 * Static application widgets and card factories.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "browsercommon.h"

#include <QFrame>
#include <QLineEdit>
#include <QWidget>

class QFocusEvent;
class QContextMenuEvent;
class QKeyEvent;
class QLayout;
class QMouseEvent;

class HomePageWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HomePageWidget(QWidget *parent = nullptr);

Q_SIGNALS:
    void backgroundClicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;
};

class AddressBarFrame : public QFrame
{
    Q_OBJECT

public:
    explicit AddressBarFrame(QWidget *parent = nullptr);
    void addBlankClickTarget(QWidget *target);

Q_SIGNALS:
    void blankClicked();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
};

class AddressLineEdit : public QLineEdit
{
    Q_OBJECT

public:
    explicit AddressLineEdit(QWidget *parent = nullptr);

Q_SIGNALS:
    void canceled();
    void focusLeft();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
};

class PathScrollArea;

class BreadcrumbFrame : public AddressBarFrame
{
    Q_OBJECT

public:
    explicit BreadcrumbFrame(QWidget *parent = nullptr);
    QWidget *contentsWidget() const;

Q_SIGNALS:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    PathScrollArea *m_scroll = nullptr;
};

class ClickableFrame : public QFrame
{
    Q_OBJECT

public:
    explicit ClickableFrame(const QUrl &url, QWidget *parent = nullptr);

Q_SIGNALS:
    void activated(const QUrl &url);
    void focused(const QUrl &url, Qt::FocusReason reason);

protected:
    void focusInEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    QUrl targetUrl() const;

private:
    QUrl m_url;
};

class DriveFrame : public ClickableFrame
{
    Q_OBJECT

public:
    explicit DriveFrame(const DriveInfo &drive,
                        bool canSafelyRemove = false,
                        bool canEject = false,
                        QWidget *parent = nullptr);
    const DriveInfo &drive() const { return m_drive; }
    bool canSafelyRemove() const { return m_canSafelyRemove; }
    bool canEject() const { return m_canEject; }

    void setCapabilities(bool canSafelyRemove, bool canEject)
    {
        m_canSafelyRemove = canSafelyRemove;
        m_canEject = canEject;
    }

Q_SIGNALS:
    void unmountRequested(const DriveInfo &drive);
    void safelyRemoveRequested(const DriveInfo &drive);
    void ejectRequested(const DriveInfo &drive);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    DriveInfo m_drive;
    bool m_canSafelyRemove = false;
    bool m_canEject = false;
};

void populateDriveContextMenu(QMenu &menu,
                              const DriveInfo &drive,
                              bool canSafelyRemove = false,
                              bool canEject = false,
                              QAction **outUnmountAction = nullptr,
                              QAction **outSafelyRemoveAction = nullptr,
                              QAction **outEjectAction = nullptr,
                              QAction **outOpenAction = nullptr,
                              QAction **outCopyPathAction = nullptr);

ClickableFrame *makeFolderCard(const QString &name,
                               const QString &path,
                               const QString &iconName,
                               QWidget *parent);
DriveFrame *makeDriveCard(const DriveInfo &drive,
                          bool canSafelyRemove,
                          bool canEject,
                          QWidget *parent);
DriveFrame *makeDriveCard(const DriveInfo &drive, QWidget *parent);
void clearLayout(QLayout *layout);
