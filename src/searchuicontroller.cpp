#include "searchuicontroller.h"

#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QSet>
#include <QSignalBlocker>
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
    return makeSearchLocation(query, scope, base, state.type, state.date, state.size);
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
    }
    if (m_widgets.stopAction && context.backend)
        m_widgets.stopAction->setVisible(context.backend->isRunning());

    const auto checkGroup = [](QActionGroup *group, int value) {
        if (!group) return;
        for (QAction *action : group->actions())
            action->setChecked(action->data().toInt() == value);
    };
    checkGroup(m_widgets.scopeGroup, state.scope);
    checkGroup(m_widgets.typeGroup, state.type);
    checkGroup(m_widgets.dateGroup, state.date);
    checkGroup(m_widgets.sizeGroup, state.size);

    if (m_widgets.scopeAction) {
        QString scopeName;
        if (state.scope == 1) scopeName = tr("Bieżący dysk", "Current drive");
        else if (state.scope == 2) scopeName = tr("Ten komputer", "This PC");
        else scopeName = tr("Bieżący folder", "Current folder");
        if (m_presentation.scopeIcon) m_widgets.scopeAction->setIcon(m_presentation.scopeIcon());
        const bool polish = m_presentation.polish && m_presentation.polish();
        m_widgets.scopeAction->setToolTip(polish
            ? QStringLiteral("Zakres wyszukiwania: %1\nKliknij lupę ze strzałką, aby zmienić zakres.").arg(scopeName)
            : QStringLiteral("Search scope: %1\nClick the magnifier arrow to change scope.").arg(scopeName));
        m_widgets.scopeAction->setEnabled(!isSearchLocation(context.location));
    }

    if (m_widgets.filterButton) {
        const int active = (state.type != 0) + (state.date != 0) + (state.size != 0);
        const bool polish = m_presentation.polish && m_presentation.polish();
        m_widgets.filterButton->setToolTip(active == 0
            ? tr("Filtry wyszukiwania", "Search filters")
            : polish ? QStringLiteral("Filtry wyszukiwania (%1 aktywne)").arg(active)
                     : QStringLiteral("Search filters (%1 active)").arg(active));
    }
}

void SearchUiController::updateProgress(SearchController &backend) const
{
    if (m_widgets.progressBar) m_widgets.progressBar->setValue(backend.progressPercent());
}

void SearchUiController::updateStatus(SearchController &backend, int visibleCount, int scope) const
{
    if (m_widgets.statusLabel)
        m_widgets.statusLabel->setText(backend.statusText(visibleCount, scope));
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
    return makeSearchLocation(searchQueryFromUrl(context.location),
        searchIntParameter(context.location, QStringLiteral("scope"), context.state->scope),
        searchBaseFromUrl(context.location), context.state->type, context.state->date, context.state->size);
}

QList<QUrl> SearchUiController::rootsForLocation(const QUrl &location,
    const PaneSearchState &state, const QList<QUrl> &driveRoots, const QUrl &home) const
{
    if (state.scope == 2) return wholeComputerSearchRoots(driveRoots);
    QUrl base = searchBaseFromUrl(location);
    if (!base.isValid()) base = home;
    return {base};
}
