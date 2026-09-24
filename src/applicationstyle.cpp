#include "applicationstyle.h"

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
    border: none;
    background: palette(base);
}

QScrollArea#sidebarScrollArea {
    border: none;
    background: palette(base);
}

QSplitter#sidebarSplitter::handle {
    background: palette(mid);
    width: 1px;
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
QPushButton#sidebarButton[dropActive="true"] {
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
QFrame#sidebarDrive[dropActive="true"] {
    border: 1px solid palette(highlight);
    background: palette(alternate-base);
}

QFrame#breadcrumbFrame {
    border: 1px solid palette(mid);
    border-radius: 6px;
    background: palette(base);
}
QFrame#breadcrumbFrame[active="true"],
QFrame#splitBreadcrumbFrame[active="true"] {
    border-color: palette(highlight);
    background: palette(alternate-base);
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
    border: 1px solid transparent;
    border-radius: 4px;
    /* Reserve room for the entire hover outline inside the breadcrumb row. */
    padding: 1px 6px;
    min-height: 18px;
    margin: 2px 0;
    background: transparent;
}
QToolButton#crumbButton:hover,
QToolButton#crumbButton:focus {
    border-color: palette(highlight);
    background: rgba(93, 126, 155, 115);
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
QFrame#primaryPaneHeader,
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
/* Hover highlighting is painted per segment by SegmentedPathButton. */
QToolButton#splitBreadcrumbButton:hover {
    background: transparent;
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
