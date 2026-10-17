/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "searchuicontroller.h"

#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include "searchprogressbar.h"
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>

namespace {
bool withinLocation(const QUrl &childRaw, const QUrl &baseRaw)
{
    const QUrl child = normalizedUrl(childRaw);
    const QUrl base = normalizedUrl(baseRaw);
    if (!child.isValid() || !base.isValid()) return false;
    if (sameLocation(child, base)) return true;
    if (child.scheme() != base.scheme()) return false;
    if (child.isLocalFile() && base.isLocalFile()) {
        const QString childPath = QDir::cleanPath(child.toLocalFile());
        const QString basePath = QDir::cleanPath(base.toLocalFile());
        if (basePath == QStringLiteral("/")) return childPath.startsWith(QLatin1Char('/'));
        return childPath.startsWith(basePath + QDir::separator());
    }
    const QString childPath = child.path();
    QString basePath = base.path();
    if (!basePath.endsWith(QLatin1Char('/'))) basePath += QLatin1Char('/');
    return childPath.startsWith(basePath);
}

int depth(const QUrl &url)
{
    return url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts).size();
}
}

QUrl SearchUiController::Request::location(const PaneSearchState &state) const
{
    SearchQuery q = state.toQuery(base);
    q.setText(query);
    q.setScope(static_cast<SearchQuery::Scope>(std::clamp(scope, 0, 2)));
    if (!q.usesAdvancedFeatures()) {
        return makeSearchLocation(query, scope, base, state.type, state.date, state.size);
    }
    return q.toUrl();
}

SearchUiController::SearchUiController(Widgets widgets, Presentation presentation)
    : m_widgets(widgets), m_presentation(std::move(presentation))
{
}

QString SearchUiController::tr(const char *pl, const char *en) const
{
    return m_presentation.translate ? m_presentation.translate(pl, en)
                                    : QString::fromUtf8(en);
}

void SearchUiController::updateControls(const Context &context) const
{
    if (!context.state) return;
    const PaneSearchState &state = *context.state;
    if (m_widgets.edit) {
        const QSignalBlocker blocker(m_widgets.edit);
        m_widgets.edit->setText(state.text);
        m_widgets.edit->setPlaceholderText(sameLocation(context.location, kThisPcUrl)
            ? tr("Szukaj na tym komputerze", "Search this computer")
            : isSearchLocation(context.location)
                ? tr("Nowe wyszukiwanie", "New search")
                : tr("Szukaj w: %1", "Search in: %1")
                    .arg(m_presentation.displayName
                        ? m_presentation.displayName(context.location)
                        : context.location.toDisplayString()));
        if (m_widgets.edit->accessibleName().isEmpty()) {
            m_widgets.edit->setAccessibleName(
                tr("Szukaj plików i folderów", "Search files and folders"));
        }
        m_widgets.edit->setAccessibleDescription(m_widgets.edit->placeholderText());
    }
    if (m_widgets.stopAction && context.backend)
        m_widgets.stopAction->setVisible(context.backend->isRunning());

    const auto checkGroup = [](QActionGroup *group, int value) {
        if (!group) return;
        for (QAction *action : group->actions())
            action->setChecked(action->data().toInt() == value);
    };
    checkGroup(m_widgets.scopeGroup, state.scope);
    checkGroup(m_widgets.nameModeGroup, static_cast<int>(state.nameMode));

    // Sync Type group and informational multiple-category action
    const int catCount = state.categories.size();
    if (catCount > 1) {
        // Multiple categories selected: do not check any single category or "All"
        if (m_widgets.typeGroup) {
            for (QAction *action : m_widgets.typeGroup->actions()) {
                action->setChecked(false);
            }
        }
        if (m_widgets.typeInfoAction) {
            const bool polish = m_presentation.polish && m_presentation.polish();
            m_widgets.typeInfoAction->setText(polish
                ? QStringLiteral("Wiele kategorii (%1)").arg(catCount)
                : QStringLiteral("Multiple categories (%1)").arg(catCount));
            m_widgets.typeInfoAction->setVisible(true);
        }
    } else {
        // 0 or 1 category: check the matching preset or "All" (type 0)
        checkGroup(m_widgets.typeGroup, state.type);
        if (m_widgets.typeInfoAction) {
            m_widgets.typeInfoAction->setVisible(false);
        }
    }

    // Sync Date group and informational custom range action
    const bool hasCustomDate = state.dateMin.has_value() || state.dateMax.has_value();
    if (hasCustomDate) {
        if (m_widgets.dateGroup) {
            for (QAction *action : m_widgets.dateGroup->actions()) {
                action->setChecked(false);
            }
        }
        if (m_widgets.dateInfoAction) {
            m_widgets.dateInfoAction->setText(tr("Niestandardowy zakres", "Custom range"));
            m_widgets.dateInfoAction->setVisible(true);
        }
    } else {
        checkGroup(m_widgets.dateGroup, state.date);
        if (m_widgets.dateInfoAction) {
            m_widgets.dateInfoAction->setVisible(false);
        }
    }

    // Sync Size group and informational custom range action
    const bool hasCustomSize = state.sizeMin.has_value() || state.sizeMax.has_value();
    if (hasCustomSize) {
        if (m_widgets.sizeGroup) {
            for (QAction *action : m_widgets.sizeGroup->actions()) {
                action->setChecked(false);
            }
        }
        if (m_widgets.sizeInfoAction) {
            m_widgets.sizeInfoAction->setText(tr("Niestandardowy zakres", "Custom range"));
            m_widgets.sizeInfoAction->setVisible(true);
        }
    } else {
        checkGroup(m_widgets.sizeGroup, state.size);
        if (m_widgets.sizeInfoAction) {
            m_widgets.sizeInfoAction->setVisible(false);
        }
    }

    bool recursive = state.recursive;
    int activeFilterCount = state.toQuery().activeFilterCount();
    if (isSearchLocation(context.location)) {
        const SearchQuery query = SearchQuery::fromUrl(context.location);
        recursive = query.recursive();
        activeFilterCount = query.activeFilterCount();
    }

    // Update scope actions text (e.g. Current folder and subfolders vs Current folder only)
    if (m_widgets.scopeGroup) {
        for (QAction *action : m_widgets.scopeGroup->actions()) {
            if (action->data().toInt() == 0) {
                action->setText(recursive
                    ? tr("Bieżący folder i podfoldery", "Current folder and subfolders")
                    : tr("Tylko bieżący folder", "Current folder only"));
            }
        }
    }

    QString scopeName;
    if (state.scope == 1) scopeName = tr("Bieżący dysk", "Current drive");
    else if (state.scope == 2) scopeName = tr("Ten komputer", "This PC");
    else scopeName = recursive
        ? tr("Bieżący folder i podfoldery", "Current folder and subfolders")
        : tr("Tylko bieżący folder", "Current folder only");

    const bool polish = m_presentation.polish && m_presentation.polish();

    if (m_widgets.scopeButton) {
        m_widgets.scopeButton->setText(scopeName);
        m_widgets.scopeButton->setToolTip(polish
            ? QStringLiteral("Zakres wyszukiwania: %1\nKliknij, aby zmienić zakres.").arg(scopeName)
            : QStringLiteral("Search scope: %1\nClick to change scope.").arg(scopeName));
        m_widgets.scopeButton->setAccessibleName(polish
            ? QStringLiteral("Zakres wyszukiwania: %1").arg(scopeName)
            : QStringLiteral("Search scope: %1").arg(scopeName));
        m_widgets.scopeButton->setAccessibleDescription(m_widgets.scopeButton->toolTip());
        m_widgets.scopeButton->setEnabled(!isSearchLocation(context.location));
    }

    if (m_widgets.scopeAction) {
        if (m_presentation.scopeIcon) m_widgets.scopeAction->setIcon(m_presentation.scopeIcon());
        m_widgets.scopeAction->setToolTip(polish
            ? QStringLiteral("Zakres wyszukiwania: %1\nKliknij, aby zmienić zakres.").arg(scopeName)
            : QStringLiteral("Search scope: %1\nClick to change scope.").arg(scopeName));
        m_widgets.scopeAction->setEnabled(!isSearchLocation(context.location));
    }

    if (m_widgets.nameModeButton) {
        QString modeText;
        QString modeToolTip;
        if (state.nameMode == SearchQuery::NameMode::Auto) {
            modeText = tr("Tryb: Auto", "Mode: Auto");
            const bool isGlob = SearchQuery::isWildcardPattern(state.text);
            if (polish) {
                modeToolTip = isGlob
                    ? QStringLiteral("Tryb dopasowania: Auto (dopasowanie wg maski glob)\nKliknij, aby zmienić tryb.")
                    : QStringLiteral("Tryb dopasowania: Auto (dopasowanie tekstowe)\nKliknij, aby zmienić tryb.");
            } else {
                modeToolTip = isGlob
                    ? QStringLiteral("Matching mode: Auto (glob pattern matching)\nClick to change mode.")
                    : QStringLiteral("Matching mode: Auto (text substring matching)\nClick to change mode.");
            }
        } else if (state.nameMode == SearchQuery::NameMode::Glob) {
            modeText = tr("Tryb: Maska glob", "Mode: Glob");
            modeToolTip = polish
                ? QStringLiteral("Tryb dopasowania: Maska glob (*, ?)\nKliknij, aby zmienić tryb.")
                : QStringLiteral("Matching mode: Glob mask (*, ?)\nClick to change mode.");
        } else {
            modeText = tr("Tryb: Tekst", "Mode: Text");
            modeToolTip = polish
                ? QStringLiteral("Tryb dopasowania: Tekst (dosłowne dopasowanie fragmentu nazwy)\nKliknij, aby zmienić tryb.")
                : QStringLiteral("Matching mode: Text (literal substring matching)\nClick to change mode.");
        }
        m_widgets.nameModeButton->setText(modeText);
        m_widgets.nameModeButton->setToolTip(modeToolTip);
        m_widgets.nameModeButton->setAccessibleName(modeText);
        m_widgets.nameModeButton->setAccessibleDescription(modeToolTip);
    }

    if (m_widgets.filterButton) {
        const QString filterBaseText = tr("Filtry", "Filters");
        if (activeFilterCount > 0) {
            m_widgets.filterButton->setText(QStringLiteral("%1 (%2)").arg(filterBaseText).arg(activeFilterCount));
            m_widgets.filterButton->setProperty("activeFilters", true);
            m_widgets.filterButton->setToolTip(polish
                ? QStringLiteral("Filtry wyszukiwania (%1 aktywne)").arg(activeFilterCount)
                : QStringLiteral("Search filters (%1 active)").arg(activeFilterCount));
        } else {
            m_widgets.filterButton->setText(filterBaseText);
            m_widgets.filterButton->setProperty("activeFilters", false);
            m_widgets.filterButton->setToolTip(tr("Filtry wyszukiwania", "Search filters"));
        }
        m_widgets.filterButton->setAccessibleName(m_widgets.filterButton->text());
        m_widgets.filterButton->setAccessibleDescription(m_widgets.filterButton->toolTip());
        m_widgets.filterButton->style()->unpolish(m_widgets.filterButton);
        m_widgets.filterButton->style()->polish(m_widgets.filterButton);
    }

    if (m_widgets.filterIndicatorAction) {
        if (activeFilterCount > 0) {
            m_widgets.filterIndicatorAction->setVisible(true);
            if (m_presentation.filterBadgeIcon) {
                m_widgets.filterIndicatorAction->setIcon(m_presentation.filterBadgeIcon(activeFilterCount));
            }
            m_widgets.filterIndicatorAction->setText(polish
                ? QStringLiteral("Filtry (%1)").arg(activeFilterCount)
                : QStringLiteral("Filters (%1)").arg(activeFilterCount));
            m_widgets.filterIndicatorAction->setToolTip(polish
                ? QStringLiteral("Filtry wyszukiwania (%1 aktywne)\nKliknij, aby otworzyć panel.").arg(activeFilterCount)
                : QStringLiteral("Search filters (%1 active)\nClick to open panel.").arg(activeFilterCount));
        } else {
            m_widgets.filterIndicatorAction->setVisible(false);
        }
        if (m_widgets.edit) {
            for (QToolButton *btn : m_widgets.edit->findChildren<QToolButton*>()) {
                if (btn->defaultAction() == m_widgets.filterIndicatorAction) {
                    btn->setAccessibleName(m_widgets.filterIndicatorAction->text());
                    btn->setAccessibleDescription(m_widgets.filterIndicatorAction->toolTip());
                }
            }
        }
    }

    if (m_widgets.clearAction && m_widgets.edit) {
        const bool hasText = !m_widgets.edit->text().isEmpty();
        m_widgets.clearAction->setVisible(hasText);
        for (QToolButton *btn : m_widgets.edit->findChildren<QToolButton*>()) {
            if (btn->defaultAction() == m_widgets.clearAction) {
                btn->setAccessibleName(m_widgets.clearAction->text());
                btn->setAccessibleDescription(m_widgets.clearAction->toolTip());
            }
        }
    }
}

void SearchUiController::updateProgress(const SearchController &backend) const
{
    if (backend.isRunning()) {
        if (m_widgets.progressFrame) m_widgets.progressFrame->show();
        if (m_widgets.progressBar) {
            m_widgets.progressBar->show();
            if (backend.isDeterminateProgress()) {
                m_widgets.progressBar->setRange(0, 100);
                m_widgets.progressBar->setValue(backend.progressPercent());
            } else {
                m_widgets.progressBar->setRange(0, 0);
            }
        }
    } else {
        if (m_widgets.progressFrame) m_widgets.progressFrame->hide();
        if (m_widgets.progressBar) {
            m_widgets.progressBar->setRange(0, 100);
            m_widgets.progressBar->setValue(backend.progressPercent());
            m_widgets.progressBar->hide();
        }
    }
}

void SearchUiController::updateStatus(const SearchController &backend, int visibleCount, int scope, bool recursive) const
{
    if (m_widgets.statusLabel)
        m_widgets.statusLabel->setText(backend.statusText(visibleCount, scope, recursive));
}

QUrl SearchUiController::bestDriveRootForUrl(const QUrl &url, const QList<QUrl> &driveRoots)
{
    QUrl best;
    int bestDepth = -1;
    for (const QUrl &candidate : driveRoots) {
        if (!candidate.isValid() || !withinLocation(url, candidate)) continue;
        const int candidateDepth = depth(candidate);
        if (candidateDepth > bestDepth) { bestDepth = candidateDepth; best = candidate; }
    }
    return best;
}

QList<QUrl> SearchUiController::wholeComputerSearchRoots(const QList<QUrl> &driveRoots)
{
    QList<QUrl> roots;
    QSet<QString> seen;
    for (const QUrl &candidate : driveRoots) {
        if (!candidate.isValid()) continue;
        const QUrl root = normalizedUrl(candidate);
        const QString key = root.toString(QUrl::FullyEncoded);
        if (seen.contains(key)) continue;
        seen.insert(key);
        roots.push_back(root);
    }
    if (roots.isEmpty()) roots.push_back(QUrl::fromLocalFile(QStringLiteral("/")));
    return roots;
}

QUrl SearchUiController::searchContextUrl(const QUrl &location, const QUrl &home)
{
    if (isSearchLocation(location)) {
        const QUrl base = searchBaseFromUrl(location);
        return base.isValid() ? base : home;
    }
    return sameLocation(location, kThisPcUrl) ? home : location;
}

SearchUiController::Request SearchUiController::requestFromUi(
    const Context &context, const QList<QUrl> &driveRoots, const QUrl &home) const
{
    Request request;
    if (!m_widgets.edit || !context.state) return request;
    request.query = m_widgets.edit->text().trimmed();
    if (request.query.isEmpty()) return request;
    request.scope = sameLocation(context.location, kThisPcUrl) ? 2 : context.state->scope;
    const QUrl searchContext = searchContextUrl(context.location, home);
    if (request.scope == 0) request.base = searchContext;
    else if (request.scope == 1) {
        request.base = bestDriveRootForUrl(searchContext, driveRoots);
        if (!request.base.isValid()) request.base = searchContext;
    }
    request.roots = request.scope == 2
        ? wholeComputerSearchRoots(driveRoots)
        : QList<QUrl>{request.base.isValid() ? request.base : home};
    return request;
}

QUrl SearchUiController::locationWithSyncedFilters(const Context &context) const
{
    if (!context.state || !isSearchLocation(context.location)) return {};
    SearchQuery query = SearchQuery::fromUrl(context.location);
    query.setRecursive(context.state->recursive);
    query.setLegacyType(context.state->type);
    query.setLegacyDate(context.state->date);
    query.setLegacySize(context.state->size);
    query.setNameMode(context.state->nameMode);
    query.setMatchCase(context.state->matchCase);
    query.setExtensions(context.state->extensions);
    query.setTargetType(context.state->targetType);
    query.setCategories(context.state->categories);
    query.setMimeTypes(context.state->mimeTypes);
    query.setDateRange(context.state->dateMin, context.state->dateMax);
    query.setSizeRange(context.state->sizeMin, context.state->sizeMax);
    if (!query.usesAdvancedFeatures()) {
        return makeSearchLocation(searchQueryFromUrl(context.location),
            searchIntParameter(context.location, QStringLiteral("scope"), context.state->scope),
            searchBaseFromUrl(context.location), context.state->type, context.state->date, context.state->size);
    }
    return query.toUrl();
}

QList<QUrl> SearchUiController::rootsForLocation(const QUrl &location,
    const PaneSearchState &state, const QList<QUrl> &driveRoots, const QUrl &home) const
{
    if (state.scope == 2) return wholeComputerSearchRoots(driveRoots);
    QUrl base = searchBaseFromUrl(location);
    if (!base.isValid()) base = home;
    return {base};
}
