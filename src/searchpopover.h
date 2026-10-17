#pragma once

#include <QFrame>
#include <QPointer>
#include <functional>

class QToolButton;
class QLineEdit;

class SearchPopover : public QFrame
{
    Q_OBJECT

public:
    static constexpr int kDefaultAnchorWidth = 320;
    static constexpr int kMinAnchorWidth = 140;

    explicit SearchPopover(QWidget *parent = nullptr);
    ~SearchPopover() override;

    QToolButton *scopeButton() const { return m_scopeButton; }
    QToolButton *nameModeButton() const { return m_nameModeButton; }
    QToolButton *filterButton() const { return m_filterButton; }
    QWidget *anchorWidget() const { return m_anchorWidget; }

    void attachAnchor(QLineEdit *searchEdit);
    void updateStableWidths(const std::function<bool()> &isPolish = nullptr);

    void showUnder(QWidget *targetWidget);
    void hidePopover();
    void togglePopover(QWidget *targetWidget);
    void reposition();
    void focusFirstButton();
    void moveButtonFocus(QToolButton *current, int delta);

    int preferredAnchorWidth() const { return m_preferredAnchorWidth; }
    void setPreferredAnchorWidth(int width);
    int calculateAnchorWidth() const;
    void updateAnchorWidth();

    int stableWidth() const { return m_stableWidth; }
    int scopeStableWidth() const { return m_scopeStableWidth; }
    int nameModeStableWidth() const { return m_nameModeStableWidth; }
    int filterStableWidth() const { return m_filterStableWidth; }
    int buttonHeight() const { return m_buttonHeight; }
    int popoverHeight() const { return m_popoverHeight; }

    bool isCompactMode() const { return width() > 0 && width() < m_stableWidth; }
    QString visibleButtonText(const QToolButton *button) const;

    void updateControlGeometry();

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyResponsiveGeometry();
    void syncTabOrder();
    int stableWidthForButton(const QToolButton *button) const;
    QToolButton *buttonForMenu(const class QMenu *menu) const;

    QToolButton *m_scopeButton = nullptr;
    QToolButton *m_nameModeButton = nullptr;
    QToolButton *m_filterButton = nullptr;
    QPointer<QWidget> m_anchorWidget;
    int m_preferredAnchorWidth = kDefaultAnchorWidth;
    int m_stableWidth = 0;
    int m_scopeStableWidth = 0;
    int m_nameModeStableWidth = 0;
    int m_filterStableWidth = 0;
    int m_buttonHeight = 0;
    int m_popoverHeight = 0;
};
