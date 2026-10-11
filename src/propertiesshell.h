/* SPDX-License-Identifier: MIT */
#pragma once
#include <QDialog>
#include <QLabel>
#include <QFont>
#include <QIcon>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>
class WrappingValueLabel : public QLabel
{
public:
    explicit WrappingValueLabel(const QString &text = QString(), QWidget *parent = nullptr)
        : QLabel(text.isEmpty() ? QStringLiteral("—") : text, parent)
    {
        setWordWrap(true);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    QSize minimumSizeHint() const override
    {
        return QSize(40, fontMetrics().height());
    }

    void setValue(const QString &text)
    {
        setText(text.isEmpty() ? QStringLiteral("—") : text);
    }
};


// Shared presentation only; data providers and write policies stay with callers.
struct PropertiesShell {
    QVBoxLayout *outer;
    QLabel *headerName;
    QTabWidget *tabs;
    PropertiesShell(QDialog *dialog, const QString &name, const QIcon &icon) {
        dialog->setMinimumSize(480, 460);
        dialog->resize(630, 680);
        outer = new QVBoxLayout(dialog);
        auto *header = new QHBoxLayout;
        auto *iconLabel = new QLabel(dialog);
        iconLabel->setObjectName(QStringLiteral("propertiesHeaderIcon"));
        iconLabel->setPixmap(icon.pixmap(64, 64));
        iconLabel->setFixedSize(72, 72);
        iconLabel->setAlignment(Qt::AlignCenter);
        headerName = new WrappingValueLabel(name, dialog);
        headerName->setObjectName(QStringLiteral("propertiesHeaderName"));
        QFont font = headerName->font();
        font.setPointSize(font.pointSize() + 3);
        font.setBold(true);
        headerName->setFont(font);
        headerName->setTextFormat(Qt::PlainText);
        header->addWidget(iconLabel);
        header->addWidget(headerName, 1);
        outer->addLayout(header);
        tabs = new QTabWidget(dialog);
        outer->addWidget(tabs, 1);
    }
    static QScrollArea *scrollArea(QWidget *parent) {
        auto *scroll = new QScrollArea(parent);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        return scroll;
    }
};
