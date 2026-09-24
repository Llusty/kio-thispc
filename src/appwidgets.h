/*
 * Static application widgets and card factories.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "browsercommon.h"

#include <QFrame>
#include <QLineEdit>

class QFocusEvent;
class QContextMenuEvent;
class QKeyEvent;
class QLayout;
class QMouseEvent;

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

class BreadcrumbFrame : public QFrame
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

protected:
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
    explicit DriveFrame(const DriveInfo &drive, QWidget *parent = nullptr);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    DriveInfo m_drive;
};

ClickableFrame *makeFolderCard(const QString &name,
                               const QString &path,
                               const QString &iconName,
                               QWidget *parent);
DriveFrame *makeDriveCard(const DriveInfo &drive, QWidget *parent);
void clearLayout(QLayout *layout);
