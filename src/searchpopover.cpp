#include "searchpopover.h"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <algorithm>

namespace {
class PopoverToolButton final : public QToolButton
{
public:
    explicit PopoverToolButton(SearchPopover *popover)
        : QToolButton(popover), m_popover(popover)
    {
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter p(this);
        QStyleOptionToolButton opt;
        initStyleOption(&opt);
        if (m_popover && !opt.text.isEmpty()) {
            opt.text = m_popover->visibleButtonText(this);
        }
        p.drawComplexControl(QStyle::CC_ToolButton, opt);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (!event) {
            QToolButton::keyPressEvent(event);
            return;
        }
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
        case Qt::Key_Down:
            if (QMenu *m = menu(); m && isEnabled()) {
                setFocus(Qt::PopupFocusReason);
                QAction *initialAction = nullptr;
                for (QAction *act : m->actions()) {
                    if (act && act->isVisible() && act->isEnabled() && !act->isSeparator()) {
                        if (act->isChecked()) {
                            initialAction = act;
                            break;
                        }
                        if (!initialAction) {
                            initialAction = act;
                        }
                    }
                }
                m->popup(mapToGlobal(QPoint(0, height())));
                if (initialAction) {
                    m->setActiveAction(initialAction);
                }
                event->accept();
                return;
            }
            break;
        case Qt::Key_Left:
        case Qt::Key_Right:
            if (m_popover && !QApplication::activePopupWidget()) {
                m_popover->moveButtonFocus(this, event->key() == Qt::Key_Right ? 1 : -1);
                event->accept();
                return;
            }
            break;
        case Qt::Key_Up:
            if (m_popover && m_popover->anchorWidget() && !QApplication::activePopupWidget()) {
                m_popover->anchorWidget()->setFocus(Qt::TabFocusReason);
                event->accept();
                return;
            }
            break;
        default:
            break;
        }
        QToolButton::keyPressEvent(event);
    }

private:
    SearchPopover *m_popover = nullptr;
};
} // namespace

SearchPopover::SearchPopover(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("searchPopoverFrame"));
    setFrameShape(QFrame::StyledPanel);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAccessibleName(tr("Ustawienia wyszukiwania", "Search settings"));
    setAccessibleDescription(tr("Zakres wyszukiwania, tryb dopasowania nazwy i filtry",
                                "Search scope, name matching mode, and filters"));
    hide();

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    m_scopeButton = new PopoverToolButton(this);
    m_scopeButton->setObjectName(QStringLiteral("searchScopeButton"));
    m_scopeButton->setPopupMode(QToolButton::InstantPopup);
    m_scopeButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_scopeButton->setIconSize(QSize(16, 16));
    m_scopeButton->setAccessibleName(tr("Zakres wyszukiwania", "Search scope"));
    layout->addWidget(m_scopeButton);

    m_nameModeButton = new PopoverToolButton(this);
    m_nameModeButton->setObjectName(QStringLiteral("searchNameModeButton"));
    m_nameModeButton->setPopupMode(QToolButton::InstantPopup);
    m_nameModeButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_nameModeButton->setIconSize(QSize(16, 16));
    m_nameModeButton->setAccessibleName(tr("Tryb dopasowania nazwy", "Name matching mode"));
    layout->addWidget(m_nameModeButton);

    m_filterButton = new PopoverToolButton(this);
    m_filterButton->setObjectName(QStringLiteral("searchFilterButton"));
    m_filterButton->setPopupMode(QToolButton::InstantPopup);
    m_filterButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_filterButton->setIconSize(QSize(16, 16));
    m_filterButton->setAccessibleName(tr("Filtry wyszukiwania", "Search filters"));
    layout->addWidget(m_filterButton);

    setTabOrder(m_scopeButton, m_nameModeButton);
    setTabOrder(m_nameModeButton, m_filterButton);

    if (qApp) {
        qApp->installEventFilter(this);
    }
}

SearchPopover::~SearchPopover()
{
    if (qApp) {
        qApp->removeEventFilter(this);
    }
    if (m_anchorWidget) {
        m_anchorWidget->removeEventFilter(this);
    }
}

void SearchPopover::syncTabOrder()
{
    setTabOrder(m_scopeButton, m_nameModeButton);
    setTabOrder(m_nameModeButton, m_filterButton);
    if (m_anchorWidget && m_anchorWidget->window() == window()) {
        QWidget::setTabOrder(m_anchorWidget, m_scopeButton);
        QWidget::setTabOrder(m_scopeButton, m_nameModeButton);
        QWidget::setTabOrder(m_nameModeButton, m_filterButton);
    }
}

void SearchPopover::attachAnchor(QLineEdit *searchEdit)
{
    if (m_anchorWidget) {
        m_anchorWidget->removeEventFilter(this);
    }
    m_anchorWidget = searchEdit;
    if (m_anchorWidget && !qApp) {
        m_anchorWidget->installEventFilter(this);
    }
    syncTabOrder();
    updateStableWidths();
}

void SearchPopover::setPreferredAnchorWidth(int width)
{
    m_preferredAnchorWidth = std::max(kMinAnchorWidth, width);
    updateAnchorWidth();
}

int SearchPopover::calculateAnchorWidth() const
{
    if (!m_anchorWidget) {
        return m_preferredAnchorWidth;
    }
    const QWidget *win = parentWidget() ? parentWidget() : m_anchorWidget->parentWidget();
    if (!win || win->width() <= 0) {
        return m_preferredAnchorWidth;
    }
    const bool inToolBar = m_anchorWidget->parentWidget()
        && m_anchorWidget->parentWidget()->inherits("QToolBar");
    const int reservedWidth = inToolBar ? 210 : 16;
    const int available = win->width() - reservedWidth;
    if (available >= m_preferredAnchorWidth) {
        return m_preferredAnchorWidth;
    }
    return std::max(kMinAnchorWidth, available);
}

void SearchPopover::updateAnchorWidth()
{
    if (!m_anchorWidget) {
        return;
    }
    const int targetWidth = calculateAnchorWidth();
    if (m_anchorWidget->width() != targetWidth
        || m_anchorWidget->minimumWidth() != targetWidth
        || m_anchorWidget->maximumWidth() != targetWidth) {
        m_anchorWidget->setFixedWidth(targetWidth);
    }
}

int SearchPopover::stableWidthForButton(const QToolButton *button) const
{
    if (button == m_scopeButton) {
        return m_scopeStableWidth;
    }
    if (button == m_nameModeButton) {
        return m_nameModeStableWidth;
    }
    if (button == m_filterButton) {
        return m_filterStableWidth;
    }
    return 0;
}

QString SearchPopover::visibleButtonText(const QToolButton *button) const
{
    if (!button) {
        return {};
    }
    const QString fullText = button->text();
    const int fullStable = stableWidthForButton(button);
    if (fullText.isEmpty() || fullStable <= 0 || button->width() >= fullStable) {
        return fullText;
    }
    const int iconSpace = button->icon().isNull() ? 0 : (button->iconSize().width() + 6);
    const int chromeSpace = 38;
    const int availTextWidth = std::max(0, button->width() - iconSpace - chromeSpace);
    const QFontMetrics fm(button->font());
    if (fm.horizontalAdvance(fullText) <= availTextWidth) {
        return fullText;
    }
    return fm.elidedText(fullText, Qt::ElideRight, availTextWidth);
}

void SearchPopover::updateStableWidths(const std::function<bool()> &isPolish)
{
    Q_UNUSED(isPolish);

    const QStringList scopeCandidates = {
        tr("Bieżący folder i podfoldery", "Current folder and subfolders"),
        tr("Tylko bieżący folder", "Current folder only"),
        tr("Bieżący dysk", "Current drive"),
        tr("Ten komputer", "This PC"),
        QStringLiteral("Bieżący folder i podfoldery"),
        QStringLiteral("Current folder and subfolders"),
        QStringLiteral("Tylko bieżący folder"),
        QStringLiteral("Current folder only"),
        QStringLiteral("Bieżący dysk"),
        QStringLiteral("Current drive"),
        QStringLiteral("Ten komputer"),
        QStringLiteral("This PC")
    };

    int maxScope = 0;
    for (const QString &text : scopeCandidates) {
        QStyleOptionToolButton opt;
        opt.initFrom(m_scopeButton);
        opt.text = text;
        opt.icon = m_scopeButton->icon();
        opt.iconSize = m_scopeButton->iconSize();
        opt.toolButtonStyle = Qt::ToolButtonTextBesideIcon;
        opt.subControls = QStyle::SC_ToolButton | QStyle::SC_ToolButtonMenu;
        opt.features = QStyleOptionToolButton::HasMenu;
        const QSize sz = m_scopeButton->style()->sizeFromContents(
            QStyle::CT_ToolButton, &opt,
            QSize(opt.fontMetrics.horizontalAdvance(text) + 48, opt.fontMetrics.height()),
            m_scopeButton);
        const int fallback = opt.fontMetrics.horizontalAdvance(text)
            + opt.iconSize.width() + 44;
        maxScope = std::max({maxScope, sz.width(), fallback});
    }

    const QStringList filterCandidates = {
        tr("Filtry", "Filters"),
        QStringLiteral("Filtry"),
        QStringLiteral("Filters"),
        QStringLiteral("Filtry (1)"),
        QStringLiteral("Filters (1)"),
        QStringLiteral("Filtry (9)"),
        QStringLiteral("Filters (9)"),
        QStringLiteral("Filtry (99)"),
        QStringLiteral("Filters (99)")
    };

    int maxFilter = 0;
    for (const QString &text : filterCandidates) {
        QStyleOptionToolButton opt;
        opt.initFrom(m_filterButton);
        opt.text = text;
        opt.icon = m_filterButton->icon();
        opt.iconSize = m_filterButton->iconSize();
        opt.toolButtonStyle = Qt::ToolButtonTextBesideIcon;
        opt.subControls = QStyle::SC_ToolButton | QStyle::SC_ToolButtonMenu;
        opt.features = QStyleOptionToolButton::HasMenu;
        const QSize sz = m_filterButton->style()->sizeFromContents(
            QStyle::CT_ToolButton, &opt,
            QSize(opt.fontMetrics.horizontalAdvance(text) + 48, opt.fontMetrics.height()),
            m_filterButton);
        const int fallback = opt.fontMetrics.horizontalAdvance(text)
            + opt.iconSize.width() + 44;
        maxFilter = std::max({maxFilter, sz.width(), fallback});
    }

    const QStringList modeCandidates = {
        tr("Tryb: Auto", "Search mode: Auto"),
        tr("Tryb: Tekst", "Search mode: Text"),
        tr("Tryb: Maska glob", "Search mode: Glob"),
        QStringLiteral("Tryb: Auto"),
        QStringLiteral("Mode: Auto"),
        QStringLiteral("Tryb: Tekst"),
        QStringLiteral("Mode: Text"),
        QStringLiteral("Tryb: Maska glob"),
        QStringLiteral("Mode: Glob")
    };

    int maxMode = 0;
    for (const QString &text : modeCandidates) {
        QStyleOptionToolButton opt;
        opt.initFrom(m_nameModeButton);
        opt.text = text;
        opt.icon = m_nameModeButton->icon();
        opt.iconSize = m_nameModeButton->iconSize();
        opt.toolButtonStyle = Qt::ToolButtonTextBesideIcon;
        opt.subControls = QStyle::SC_ToolButton | QStyle::SC_ToolButtonMenu;
        opt.features = QStyleOptionToolButton::HasMenu;
        const QSize sz = m_nameModeButton->style()->sizeFromContents(
            QStyle::CT_ToolButton, &opt,
            QSize(opt.fontMetrics.horizontalAdvance(text) + 48, opt.fontMetrics.height()),
            m_nameModeButton);
        const int fallback = opt.fontMetrics.horizontalAdvance(text)
            + opt.iconSize.width() + 44;
        maxMode = std::max({maxMode, sz.width(), fallback});
    }

    m_scopeStableWidth = maxScope;
    m_nameModeStableWidth = maxMode;
    m_filterStableWidth = maxFilter;

    // Calculate target height for buttons to visually match the search line edit and current font metrics
    const int fontHeight = m_scopeButton->fontMetrics().height();
    int targetHeight = std::max(32, fontHeight + 14);
    if (m_anchorWidget) {
        const int anchorHeight = m_anchorWidget->height();
        const int anchorHint = m_anchorWidget->sizeHint().height();
        targetHeight = std::max({targetHeight, anchorHeight, anchorHint});
    }

    m_buttonHeight = targetHeight;

    const int horizontalMargins = layout()->contentsMargins().left() + layout()->contentsMargins().right();
    const int verticalMargins = layout()->contentsMargins().top() + layout()->contentsMargins().bottom();
    const int spacing = layout()->spacing();

    m_stableWidth = m_scopeStableWidth + m_nameModeStableWidth + m_filterStableWidth + (2 * spacing) + horizontalMargins;
    m_popoverHeight = m_buttonHeight + verticalMargins;

    updateAnchorWidth();
    applyResponsiveGeometry();
}

void SearchPopover::applyResponsiveGeometry()
{
    if (m_stableWidth <= 0 || m_buttonHeight <= 0) {
        return;
    }

    const int horizontalMargins = layout()->contentsMargins().left() + layout()->contentsMargins().right();
    const int spacing = layout()->spacing();

    int maxAvailableWidth = m_stableWidth;
    if (parentWidget() && parentWidget()->width() > 32) {
        maxAvailableWidth = parentWidget()->width() - 16;
    }

    if (maxAvailableWidth >= m_stableWidth) {
        m_scopeButton->setFixedSize(m_scopeStableWidth, m_buttonHeight);
        m_nameModeButton->setFixedSize(m_nameModeStableWidth, m_buttonHeight);
        m_filterButton->setFixedSize(m_filterStableWidth, m_buttonHeight);
        setFixedSize(m_stableWidth, m_popoverHeight);
        return;
    }

    const int minButtonW = 56;
    const int targetTotalW = std::max(horizontalMargins + (2 * spacing) + 72, maxAvailableWidth);
    const int innerW = std::max(72, targetTotalW - horizontalMargins - (2 * spacing));
    const int effectiveMinButtonW = std::min(minButtonW, innerW / 3);
    const int totalStableButtons = std::max(1, m_scopeStableWidth + m_nameModeStableWidth + m_filterStableWidth);

    int filterW = std::max(effectiveMinButtonW, (innerW * m_filterStableWidth) / totalStableButtons);
    int modeW = std::max(effectiveMinButtonW, (innerW * m_nameModeStableWidth) / totalStableButtons);
    int scopeW = std::max(effectiveMinButtonW, innerW - filterW - modeW);
    if (scopeW + modeW + filterW > innerW) {
        scopeW = innerW / 3;
        modeW = innerW / 3;
        filterW = innerW - scopeW - modeW;
    }

    m_scopeButton->setFixedSize(scopeW, m_buttonHeight);
    m_nameModeButton->setFixedSize(modeW, m_buttonHeight);
    m_filterButton->setFixedSize(filterW, m_buttonHeight);
    setFixedSize(scopeW + modeW + filterW + (2 * spacing) + horizontalMargins, m_popoverHeight);
}

void SearchPopover::updateControlGeometry()
{
    updateStableWidths();
}

void SearchPopover::showUnder(QWidget *targetWidget)
{
    if (targetWidget && targetWidget != m_anchorWidget) {
        if (m_anchorWidget) {
            m_anchorWidget->removeEventFilter(this);
        }
        m_anchorWidget = targetWidget;
        if (m_anchorWidget && !qApp) {
            m_anchorWidget->installEventFilter(this);
        }
    }
    syncTabOrder();
    updateStableWidths();
    reposition();
    raise();
    show();
}

void SearchPopover::hidePopover()
{
    hide();
}

void SearchPopover::togglePopover(QWidget *targetWidget)
{
    if (isVisible()) {
        hidePopover();
    } else {
        showUnder(targetWidget);
    }
}

void SearchPopover::focusFirstButton()
{
    for (QToolButton *btn : {m_scopeButton, m_nameModeButton, m_filterButton}) {
        if (btn && btn->isEnabled()) {
            btn->setFocus(Qt::TabFocusReason);
            return;
        }
    }
}

void SearchPopover::moveButtonFocus(QToolButton *current, int delta)
{
    QList<QToolButton *> enabledButtons;
    for (QToolButton *btn : {m_scopeButton, m_nameModeButton, m_filterButton}) {
        if (btn && btn->isEnabled()) {
            enabledButtons.append(btn);
        }
    }
    if (enabledButtons.isEmpty()) {
        return;
    }
    const int idx = enabledButtons.indexOf(current);
    if (idx < 0) {
        enabledButtons.first()->setFocus(Qt::TabFocusReason);
        return;
    }
    const int nextIdx = std::clamp(idx + delta, 0, static_cast<int>(enabledButtons.size()) - 1);
    enabledButtons.at(nextIdx)->setFocus(Qt::TabFocusReason);
}

QToolButton *SearchPopover::buttonForMenu(const QMenu *menu) const
{
    if (!menu) {
        return nullptr;
    }
    for (QToolButton *btn : {m_scopeButton, m_nameModeButton, m_filterButton}) {
        if (!btn || !btn->menu()) {
            continue;
        }
        if (btn->menu() == menu) {
            return btn;
        }
        const auto subMenus = btn->menu()->findChildren<QMenu *>();
        if (subMenus.contains(const_cast<QMenu *>(menu))) {
            return btn;
        }
    }
    return nullptr;
}

void SearchPopover::reposition()
{
    if (!m_anchorWidget || !parentWidget()) {
        return;
    }

    updateAnchorWidth();
    applyResponsiveGeometry();

    const QPoint anchorPos = m_anchorWidget->mapTo(
        parentWidget(), QPoint(0, m_anchorWidget->height() + 2));
    const int anchorRight = anchorPos.x() + m_anchorWidget->width();
    const int popWidth = width();
    const int parentWidth = parentWidget()->width();

    int x = anchorPos.x();
    if (anchorPos.x() + (m_anchorWidget->width() / 2) > parentWidth / 2
        || x + popWidth > parentWidth - 8) {
        x = anchorRight - popWidth;
    }

    if (x + popWidth > parentWidth - 8) {
        x = parentWidth - 8 - popWidth;
    }
    if (x < 8) {
        x = 8;
    }

    int y = anchorPos.y();
    const int parentHeight = parentWidget()->height();
    if (parentHeight > height() + 16 && y + height() > parentHeight - 8) {
        y = std::max(8, parentHeight - 8 - height());
    }

    move(x, y);
}

void SearchPopover::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (!event) {
        return;
    }
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::StyleChange:
    case QEvent::ApplicationFontChange:
    case QEvent::ScreenChangeInternal:
    case QEvent::DevicePixelRatioChange:
        m_scopeButton->setFont(font());
        m_nameModeButton->setFont(font());
        m_filterButton->setFont(font());
        updateStableWidths();
        if (isVisible()) {
            reposition();
        }
        break;
    default:
        break;
    }
}

bool SearchPopover::eventFilter(QObject *watched, QEvent *event)
{
    if (auto *menu = qobject_cast<QMenu *>(watched)) {
        if (QToolButton *ownerBtn = buttonForMenu(menu)) {
            if (event->type() == QEvent::KeyPress) {
                auto *keyEvent = static_cast<QKeyEvent *>(event);
                if (keyEvent->key() == Qt::Key_Space
                    && !menu->style()->styleHint(QStyle::SH_Menu_SpaceActivatesItem, nullptr, menu)) {
                    if (QAction *act = menu->activeAction();
                        act && act->isEnabled() && !act->isSeparator()) {
                        QKeyEvent enterPress(QEvent::KeyPress, Qt::Key_Return, keyEvent->modifiers());
                        QApplication::sendEvent(menu, &enterPress);
                        return true;
                    }
                }
            } else if (event->type() == QEvent::Hide) {
                if (menu == ownerBtn->menu() && isVisible()) {
                    if (window() && window()->isVisible()) {
                        ownerBtn->setFocus(Qt::PopupFocusReason);
                    }
                }
            }
        }
    }

    if (watched == m_anchorWidget) {
        if (event->type() == QEvent::MouseButtonPress) {
            if (!isVisible()) {
                showUnder(m_anchorWidget);
            }
            return false;
        }
        if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape && isVisible() && !QApplication::activePopupWidget()) {
                hidePopover();
                return true;
            }
            if (keyEvent->key() == Qt::Key_Down) {
                if ((keyEvent->modifiers() & Qt::AltModifier) && !isVisible()) {
                    showUnder(m_anchorWidget);
                    return true;
                }
                if (isVisible()
                    && (keyEvent->modifiers() == Qt::NoModifier
                        || (keyEvent->modifiers() & Qt::AltModifier))) {
                    focusFirstButton();
                    return true;
                }
            }
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
                hidePopover();
                return false;
            }
        }
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
            if (isVisible()) {
                reposition();
            }
        }
    }

    if (event->type() == QEvent::FontChange
        || event->type() == QEvent::StyleChange
        || event->type() == QEvent::ScreenChangeInternal
        || event->type() == QEvent::DevicePixelRatioChange) {
        if (watched == parentWidget() || watched == m_anchorWidget) {
            updateStableWidths();
            if (isVisible()) {
                reposition();
            }
        }
    }

    if (isVisible() && event->type() == QEvent::KeyPress) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && (widget == this || isAncestorOf(widget))) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape && !QApplication::activePopupWidget()) {
                hidePopover();
                if (m_anchorWidget) {
                    m_anchorWidget->setFocus();
                }
                return true;
            }
        }
    }

    if (event->type() == QEvent::Resize) {
        if (watched == parentWidget()
            || (m_anchorWidget && watched == m_anchorWidget->parentWidget())) {
            updateAnchorWidth();
            if (isVisible()) {
                reposition();
            } else {
                applyResponsiveGeometry();
            }
        }
    }

    if (isVisible() && event->type() == QEvent::MouseButtonPress) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget) {
            if (widget == this || isAncestorOf(widget)) {
                return false;
            }
            if (m_anchorWidget && (widget == m_anchorWidget || m_anchorWidget->isAncestorOf(widget))) {
                return false;
            }
            if (qobject_cast<QMenu *>(widget) || widget->inherits("QMenu")
                || qobject_cast<QMenu *>(widget->window())) {
                return false;
            }
            if (QApplication::activePopupWidget()) {
                return false;
            }
            hidePopover();
        }
    }

    return QFrame::eventFilter(watched, event);
}
