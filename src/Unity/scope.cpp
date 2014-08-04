/*
 * Copyright (C) 2013 Canonical, Ltd.
 *
 * Authors:
 *  Michal Hruby <michal.hruby@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// Self
#include "scope.h"

// local
#include "categories.h"
#include "collectors.h"
#include "previewstack.h"
#include "utils.h"
#include "scopes.h"
#include "settingsmodel.h"

// Qt
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>
#include <QtGui/QDesktopServices>
#include <QQmlEngine>
#include <QEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QScopedPointer>
#include <QFileInfo>
#include <QDir>
#include <QLocale>

#include <libintl.h>

#include <unity/scopes/ListenerBase.h>
#include <unity/scopes/CannedQuery.h>
#include <unity/scopes/OptionSelectorFilter.h>
#include <unity/scopes/CategorisedResult.h>
#include <unity/scopes/QueryCtrl.h>
#include <unity/scopes/PreviewWidget.h>
#include <unity/scopes/SearchMetadata.h>
#include <unity/scopes/ActionMetadata.h>

namespace scopes_ng
{

using namespace unity;

const int AGGREGATION_TIMEOUT = 110;
const int CLEAR_TIMEOUT = 240;
const int RESULTS_TTL_SMALL = 30000; // 30 seconds
const int RESULTS_TTL_MEDIUM = 300000; // 5 minutes
const int RESULTS_TTL_LARGE = 3600000; // 1 hour

Scope::Scope(QObject *parent) : unity::shell::scopes::ScopeInterface(parent)
    , m_formFactor("phone")
    , m_isActive(false)
    , m_searchInProgress(false)
    , m_resultsDirty(false)
    , m_delayedClear(false)
    , m_hasNavigation(false)
    , m_hasAltNavigation(false)
    , m_searchController(new CollectionController)
    , m_activationController(new CollectionController)
    , m_status(Status::Okay)
{
    m_categories.reset(new Categories(this));

    m_settings = QGSettings::isSchemaInstalled("com.canonical.Unity.Lenses") ? new QGSettings("com.canonical.Unity.Lenses", QByteArray(), this) : nullptr;
    QObject::connect(m_settings, &QGSettings::changed, this, &Scope::internetFlagChanged);

    setScopesInstance(qobject_cast<scopes_ng::Scopes*>(parent));

    m_aggregatorTimer.setSingleShot(true);
    QObject::connect(&m_aggregatorTimer, &QTimer::timeout, this, &Scope::flushUpdates);
    m_clearTimer.setSingleShot(true);
    QObject::connect(&m_clearTimer, &QTimer::timeout, this, &Scope::flushUpdates);
    m_invalidateTimer.setSingleShot(true);
    m_invalidateTimer.setTimerType(Qt::VeryCoarseTimer);
    QObject::connect(&m_invalidateTimer, &QTimer::timeout, this, &Scope::invalidateResults);
}

Scope::~Scope()
{
}

void Scope::processSearchChunk(PushEvent* pushEvent)
{
    CollectorBase::Status status;
    QList<std::shared_ptr<scopes::CategorisedResult>> results;
    scopes::Department::SCPtr rootDepartment;
    scopes::OptionSelectorFilter::SCPtr sortOrderFilter;
    scopes::FilterState filterState;

    status = pushEvent->collectSearchResults(results, rootDepartment, sortOrderFilter, filterState);
    if (status == CollectorBase::Status::CANCELLED) {
        return;
    }

    m_rootDepartment = rootDepartment;
    m_sortOrderFilter = sortOrderFilter;
    m_receivedFilterState = filterState;

    if (m_cachedResults.empty()) {
        m_cachedResults.swap(results);
    } else {
        m_cachedResults.append(results);
    }

    if (status == CollectorBase::Status::INCOMPLETE) {
        if (!m_aggregatorTimer.isActive()) {
            // the longer we've been waiting for the results, the shorter the timeout
            qint64 inProgressMs = pushEvent->msecsSinceStart();
            double mult = 1.0 / std::max(1, static_cast<int>((inProgressMs / 150) + 1));
            m_aggregatorTimer.start(AGGREGATION_TIMEOUT * mult);
        }
    } else { // status in [FINISHED, ERROR]
        m_aggregatorTimer.stop();

        flushUpdates();

        setSearchInProgress(false);
        setStatus(status == CollectorBase::Status::FINISHED ? Status::Okay : Status::Unknown);

        // Don't schedule a refresh if the query suffered an error
        if (status == CollectorBase::Status::FINISHED) {
            startTtlTimer();
        }
    }
}

bool Scope::event(QEvent* ev)
{
    if (ev->type() == PushEvent::eventType) {
        PushEvent* pushEvent = static_cast<PushEvent*>(ev);

        switch (pushEvent->type()) {
            case PushEvent::SEARCH:
                processSearchChunk(pushEvent);
                return true;
            case PushEvent::ACTIVATION: {
                std::shared_ptr<scopes::ActivationResponse> response;
                std::shared_ptr<scopes::Result> result;
                pushEvent->collectActivationResponse(response, result);
                if (response) {
                    handleActivation(response, result);
                }
                return true;
            }
            default:
                qWarning("Unknown PushEvent type!");
                return false;
        }
    }
    return QObject::event(ev);
}

void Scope::handleActivation(std::shared_ptr<scopes::ActivationResponse> const& response, scopes::Result::SPtr const& result)
{
    switch (response->status()) {
        case scopes::ActivationResponse::NotHandled:
            activateUri(QString::fromStdString(result->uri()));
            break;
        case scopes::ActivationResponse::HideDash:
            Q_EMIT hideDash();
            break;
        case scopes::ActivationResponse::ShowDash:
            Q_EMIT showDash();
            break;
        case scopes::ActivationResponse::ShowPreview:
            Q_EMIT previewRequested(QVariant::fromValue(result));
            break;
         case scopes::ActivationResponse::PerformQuery:
            executeCannedQuery(response->query(), true);
            break;
        default:
            break;
    }
}

void Scope::metadataRefreshed()
{
    std::shared_ptr<scopes::ActivationResponse> response;
    response.swap(m_delayedActivation);

    if (!response) {
        return;
    }

    if (response->status() == scopes::ActivationResponse::PerformQuery) {
        executeCannedQuery(response->query(), false);
    }
}

void Scope::internetFlagChanged(QString const& key)
{
    if (key != "remoteContentSearch") {
        return;
    }

    invalidateResults();
}

void Scope::executeCannedQuery(unity::scopes::CannedQuery const& query, bool allowDelayedActivation)
{
    if (!m_scopesInstance) {
        qWarning("Scope instance %p doesn't have associated Scopes instance", static_cast<void*>(this));
        return;
    }

    QString scopeId(QString::fromStdString(query.scope_id()));
    QString searchString(QString::fromStdString(query.query_string()));
    QString departmentId(QString::fromStdString(query.department_id()));

    Scope* scope = nullptr;
    if (scopeId == id()) {
        scope = this;
    } else {
        // figure out if this scope is already favourited
        scope = m_scopesInstance->getScopeById(scopeId);
    }

    if (scope != nullptr) {
        scope->setCurrentNavigationId(departmentId);
        scope->setFilterState(query.filter_state());
        scope->setSearchQuery(searchString);
        // FIXME: implement better way to do multiple changes to search props and dispatch single search
        if (!scope->searchInProgress()) {
            scope->invalidateResults();
        }
        if (scope != this) Q_EMIT gotoScope(scopeId);
    } else {
        // create temp dash page
        auto meta_sptr = m_scopesInstance->getCachedMetadata(scopeId);
        if (meta_sptr) {
            scope = new scopes_ng::Scope(this);
            scope->setScopeData(*meta_sptr);
            scope->setScopesInstance(m_scopesInstance);
            scope->setCurrentNavigationId(departmentId);
            scope->setFilterState(query.filter_state());
            scope->setSearchQuery(searchString);
            m_tempScopes.insert(scope);
            Q_EMIT openScope(scope);
        } else if (allowDelayedActivation) {
            // request registry refresh to get the missing metadata
            m_delayedActivation = std::make_shared<scopes::ActivationResponse>(query);
            m_scopesInstance->refreshScopeMetadata();
        } else {
            qWarning("Unable to find scope \"%s\" after metadata refresh", query.scope_id().c_str());
        }
    }
}

void Scope::flushUpdates()
{
    if (m_delayedClear) {
        // TODO: here we could do resultset diffs
        m_categories->clearAll();
        m_delayedClear = false;
    }

    if (m_clearTimer.isActive()) {
        m_clearTimer.stop();
    }

    if (m_status != Status::Okay) {
        setStatus(Status::Okay);
    }
    processResultSet(m_cachedResults); // clears the result list

    // process departments
    if (m_rootDepartment && m_rootDepartment != m_lastRootDepartment) {
        // build / append to the tree
        DepartmentNode* node = nullptr;
        if (m_departmentTree) {
            scopes::Department::SCPtr updateNode(m_rootDepartment);
            QString departmentId(QString::fromStdString(updateNode->id()));
            node = m_departmentTree->findNodeById(departmentId);
            if (node == nullptr) {
                node = m_departmentTree.data();
            } else {
                // we have the node in our tree, try to find the minimal subtree to update
                updateNode = findUpdateNode(node, updateNode);
                if (updateNode) {
                    node = m_departmentTree->findNodeById(QString::fromStdString(updateNode->id()));
                }
            }
            if (updateNode) {
                node->initializeForDepartment(updateNode);
            }
            // as far as we know, this is the root, re-initializing might have unset the flag
            m_departmentTree->setIsRoot(true);

            // update corresponding models
            updateNavigationModels(m_departmentTree.data(), m_departmentModels, m_currentNavigationId);
        } else {
            m_departmentTree.reset(new DepartmentNode);
            m_departmentTree->initializeForDepartment(m_rootDepartment);
            // as far as we know, this is the root, changing our mind later
            // is better than pretending it isn't
            m_departmentTree->setIsRoot(true);
        }
    }

    m_lastRootDepartment = m_rootDepartment;

    bool containsDepartments = m_rootDepartment.get() != nullptr;
    // design decision - no navigation when doing searches
    containsDepartments &= m_searchQuery.isEmpty();

    if (containsDepartments != m_hasNavigation) {
        m_hasNavigation = containsDepartments;
        Q_EMIT hasNavigationChanged();
    }

    if (!containsDepartments && !m_currentNavigationId.isEmpty()) {
        m_currentNavigationId = "";
        Q_EMIT currentNavigationIdChanged();
    }

    // process the alt navigation (sort order filter)
    QString currentAltNav(m_currentAltNavigationId);

    if (m_sortOrderFilter && m_sortOrderFilter != m_lastSortOrderFilter) {
        // build the nodes
        m_altNavTree.reset(new DepartmentNode);
        m_altNavTree->initializeForFilter(m_sortOrderFilter);

        if (m_sortOrderFilter->has_active_option(m_receivedFilterState)) {
            auto active_options = m_sortOrderFilter->active_options(m_receivedFilterState);
            scopes::FilterOption::SCPtr active_option = *active_options.begin();
            if (active_option) {
                currentAltNav = QString::fromStdString(active_option->id());
            }
        }
    }

    m_lastSortOrderFilter = m_sortOrderFilter;

    bool containsAltNav = m_sortOrderFilter.get() != nullptr;
    // design decision - no navigation when doing searches
    containsAltNav &= m_searchQuery.isEmpty();

    if (containsAltNav != m_hasAltNavigation) {
        m_hasAltNavigation = containsAltNav;
        Q_EMIT hasAltNavigationChanged();
    }

    if (currentAltNav != m_currentAltNavigationId) {
        m_currentAltNavigationId = currentAltNav;
        Q_EMIT currentAltNavigationIdChanged();

        // update the alt navigation models
        updateNavigationModels(m_altNavTree.data(), m_altNavModels, m_currentAltNavigationId);
    }
}

void Scope::updateNavigationModels(DepartmentNode* rootNode, QMultiMap<QString, Department*>& navigationModels, QString const& activeNavigation)
{
    DepartmentNode* parentNode = nullptr;
    DepartmentNode* node = rootNode->findNodeById(activeNavigation);
    if (node != nullptr) {
        auto it = navigationModels.find(activeNavigation);
        while (it != navigationModels.end() && it.key() == activeNavigation) {
            it.value()->loadFromDepartmentNode(node);
            ++it;
        }
        // if this node is a leaf, we need to update models for the parent
        parentNode = node->isLeaf() ? node->parent() : nullptr;
    }
    if (parentNode != nullptr) {
        auto it = navigationModels.find(parentNode->id());
        while (it != navigationModels.end() && it.key() == parentNode->id()) {
            it.value()->markSubdepartmentActive(activeNavigation);
            ++it;
        }
    }
}

scopes::Department::SCPtr Scope::findUpdateNode(DepartmentNode* node, scopes::Department::SCPtr const& scopeNode)
{
    if (node == nullptr || node->id() != QString::fromStdString(scopeNode->id())) return scopeNode;

    // are all the children in our cache?
    QStringList cachedChildrenIds;
    Q_FOREACH(DepartmentNode* child, node->childNodes()) {
        cachedChildrenIds << child->id();
    }
    auto subdeps = scopeNode->subdepartments();
    QMap<QString, scopes::Department::SCPtr> childIdMap;
    for (auto it = subdeps.begin(); it != subdeps.end(); ++it) {
        QString childId = QString::fromStdString((*it)->id());
        childIdMap.insert(childId, *it);
        if (!cachedChildrenIds.contains(childId)) {
            return scopeNode;
        }
    }

    scopes::Department::SCPtr firstMismatchingChild;

    Q_FOREACH(DepartmentNode* child, node->childNodes()) {
        scopes::Department::SCPtr scopeChildNode(childIdMap[child->id()]);
        // the cache might have more data than the node, should we consider that bad?
        if (!scopeChildNode) {
            continue;
        }
        scopes::Department::SCPtr updateNode = findUpdateNode(child, scopeChildNode);
        if (updateNode) {
            if (!firstMismatchingChild) {
                firstMismatchingChild = updateNode;
            } else {
                // there are multiple mismatching children, update the entire node
                return scopeNode;
            }
        }
    }

    return firstMismatchingChild; // will be nullptr if everything matches
}

scopes::Department::SCPtr Scope::findDepartmentById(scopes::Department::SCPtr const& root, std::string const& id)
{
    if (root->id() == id) return root;

    auto sub_deps = root->subdepartments();
    for (auto it = sub_deps.begin(); it != sub_deps.end(); ++it) {
        if ((*it)->id() == id) {
            return *it;
        } else {
            auto node = findDepartmentById(*it, id);
            if (node) return node;
        }
    }

    return nullptr;
}

void Scope::processResultSet(QList<std::shared_ptr<scopes::CategorisedResult>>& result_set)
{
    if (result_set.count() == 0) return;

    // this will keep the list of categories in order
    QList<scopes::Category::SCPtr> categories;

    // split the result_set by category_id
    QMap<std::string, QList<std::shared_ptr<scopes::CategorisedResult>>> category_results;
    while (!result_set.empty()) {
        auto result = result_set.takeFirst();
        if (!category_results.contains(result->category()->id())) {
            categories.append(result->category());
        }
        category_results[result->category()->id()].append(std::move(result));
    }

    Q_FOREACH(scopes::Category::SCPtr const& category, categories) {
        ResultsModel* category_model = m_categories->lookupCategory(category->id());
        if (category_model == nullptr) {
            category_model = new ResultsModel(m_categories.data());
            category_model->setCategoryId(QString::fromStdString(category->id()));
            category_model->addResults(category_results[category->id()]);
            m_categories->registerCategory(category, category_model);
        } else {
            // FIXME: only update when we know it's necessary
            m_categories->registerCategory(category, nullptr);
            category_model->addResults(category_results[category->id()]);
            m_categories->updateResultCount(category_model);
        }
    }
}

scopes::ScopeProxy Scope::proxy() const
{
    return m_proxy;
}

scopes::ScopeProxy Scope::proxy_for_result(scopes::Result::SPtr const& result) const
{
    return result->target_scope_proxy();
}

void Scope::invalidateLastSearch()
{
    m_searchController->invalidate();
    if (m_aggregatorTimer.isActive()) {
        m_aggregatorTimer.stop();
    }
    m_cachedResults.clear();
}

void Scope::startTtlTimer()
{
    if (m_scopeMetadata) {
        int ttl = 0;
        switch (m_scopeMetadata->results_ttl_type()) {
        case (scopes::ScopeMetadata::ResultsTtlType::None):
            break;
        case (scopes::ScopeMetadata::ResultsTtlType::Small):
            ttl = RESULTS_TTL_SMALL;
            break;
        case (scopes::ScopeMetadata::ResultsTtlType::Medium):
            ttl = RESULTS_TTL_MEDIUM;
            break;
        case (scopes::ScopeMetadata::ResultsTtlType::Large):
            ttl = RESULTS_TTL_LARGE;
            break;
        }
        if (ttl > 0) {
            if (qEnvironmentVariableIsSet("UNITY_SCOPES_RESULTS_TTL_OVERRIDE")) {
                ttl = QString::fromUtf8(
                        qgetenv("UNITY_SCOPES_RESULTS_TTL_OVERRIDE")).toInt();
            }
            m_invalidateTimer.start(ttl);
        }
    }
}

void Scope::setScopesInstance(Scopes* scopes)
{
    if (m_metadataConnection) {
        QObject::disconnect(m_metadataConnection);
    }

    m_scopesInstance = scopes;
    if (m_scopesInstance) {
        m_metadataConnection = QObject::connect(scopes, &Scopes::metadataRefreshed, this, &Scope::metadataRefreshed);
        m_locationService = m_scopesInstance->locationService();
        // TODO Notify the user the the location has changed
        // connect(m_locationService.data(), &LocationService::locationChanged, this, &Scope::invalidateResults);
    }
}

void Scope::setSearchInProgress(bool searchInProgress)
{
    if (m_searchInProgress != searchInProgress) {
        m_searchInProgress = searchInProgress;
        Q_EMIT searchInProgressChanged();
    }
}

void Scope::setStatus(shell::scopes::ScopeInterface::Status status)
{
    if (m_status != status) {
        m_status = status;
        Q_EMIT statusChanged();
    }
}

void Scope::setCurrentNavigationId(QString const& id)
{
    if (m_currentNavigationId != id) {
        m_currentNavigationId = id;
        Q_EMIT currentNavigationIdChanged();
    }
}

void Scope::setFilterState(scopes::FilterState const& filterState)
{
    m_filterState = filterState;
}

void Scope::dispatchSearch()
{
    invalidateLastSearch();
    m_delayedClear = true;
    m_clearTimer.start(CLEAR_TIMEOUT);
    /* There are a few objects associated with searches:
     * 1) SearchResultReceiver    2) ResultCollector    3) PushEvent
     *
     * SearchResultReceiver is associated with the search and has methods that get called
     * by the scopes framework when results / categories / annotations are received.
     * Since the notification methods (push(...)) of SearchResultReceiver are called
     * from different thread(s), it uses ResultCollector to collect multiple results
     * in a thread-safe manner.
     * Once a couple of results are collected, the collector is sent via a PushEvent
     * to the UI thread, where it is processed. When the results are collected by the UI thread,
     * the collector continues to collect more results, and uses another PushEvent to post them.
     *
     * If a new query is submitted the previous one is marked as cancelled by invoking
     * SearchResultReceiver::invalidate() and any PushEvent that is waiting to be processed
     * will be discarded as the collector will also be marked as invalid.
     * The new query will have new instances of SearchResultReceiver and ResultCollector.
     */

    if (m_resultsDirty)
    {
        m_resultsDirty = false;
        resultsDirtyChanged();
    }

    setSearchInProgress(true);

    if (m_proxy) {
        scopes::SearchMetadata meta(QLocale::system().name().toStdString(), m_formFactor.toStdString());
        if (m_settings) {
            QVariant remoteSearch(m_settings->get("remote-content-search"));
            if (remoteSearch.toString() == QString("none")) {
                meta["no-internet"] = true;
            }
        }
        try {
            // TODO Verify that the scope is allowed to access the location data
            if (m_scopeMetadata && m_scopeMetadata->location_data_needed())
            {
                meta.set_location(m_locationService->location());
            }
        }
        catch (std::domain_error& e)
        {
        }
        scopes::SearchListenerBase::SPtr listener(new SearchResultReceiver(this));
        m_searchController->setListener(listener);
        try {
            scopes::QueryCtrlProxy controller = m_proxy->search(m_searchQuery.toStdString(), m_currentNavigationId.toStdString(), m_filterState, meta, listener);
            m_searchController->setController(controller);
        } catch (std::exception& e) {
            qWarning("Caught an error from create_query(): %s", e.what());
        } catch (...) {
            qWarning("Caught an error from create_query()");
        }
    }

    if (!m_searchController->isValid()) {
        // something went wrong, reset search state
        setSearchInProgress(false);
    }
}

void Scope::setScopeData(scopes::ScopeMetadata const& data)
{
    m_scopeMetadata = std::make_shared<scopes::ScopeMetadata>(data);
    m_proxy = data.proxy();

    QVariant converted(scopeVariantToQVariant(scopes::Variant(m_scopeMetadata->appearance_attributes())));
    m_customizations = converted.toMap();
    Q_EMIT customizationsChanged();

    try
    {
        scopes::Variant settings_definitions;
        settings_definitions = m_scopeMetadata->settings_definitions();
        m_settingsModel.reset(
                new SettingsModel(QDir::home().filePath(".local/share"), id(),
                        scopeVariantToQVariant(settings_definitions), this));
    }
    catch (unity::scopes::NotFoundException&)
    {
        // If there's no settings data
        m_settingsModel.reset();
    }
    Q_EMIT settingsChanged();
}

QString Scope::id() const
{
    return QString::fromStdString(m_scopeMetadata ? m_scopeMetadata->scope_id() : "");
}

QString Scope::name() const
{
    return QString::fromStdString(m_scopeMetadata ? m_scopeMetadata->display_name() : "");
}

QString Scope::iconHint() const
{
    std::string icon;
    try {
        if (m_scopeMetadata) {
            icon = m_scopeMetadata->icon();
        }
    } catch (...) {
        // throws if the value isn't set, safe to ignore
    }

    return QString::fromStdString(icon);
}

QString Scope::description() const
{
    return QString::fromStdString(m_scopeMetadata ? m_scopeMetadata->description() : "");
}

QString Scope::searchHint() const
{
    std::string search_hint;
    try {
        if (m_scopeMetadata) {
            search_hint = m_scopeMetadata->search_hint();
        }
    } catch (...) {
        // throws if the value isn't set, safe to ignore
    }

    return QString::fromStdString(search_hint);
}

bool Scope::searchInProgress() const
{
    return m_searchInProgress;
}

unity::shell::scopes::ScopeInterface::Status Scope::status() const
{
    return m_status;
}

bool Scope::favorite() const
{
    return true;
}

QString Scope::shortcut() const
{
    std::string hotkey;
    try {
        if (m_scopeMetadata) {
            hotkey = m_scopeMetadata->hot_key();
        }
    } catch (...) {
        // throws if the value isn't set, safe to ignore
    }

    return QString::fromStdString(hotkey);
}

unity::shell::scopes::CategoriesInterface* Scope::categories() const
{
    return m_categories.data();
}

unity::shell::scopes::SettingsModelInterface* Scope::settings() const
{
    return m_settingsModel.data();
}

/*
Filters* Scope::filters() const
{
    return m_filters.get();
}
*/

unity::shell::scopes::NavigationInterface* Scope::getNavigation(QString const& navId)
{
    if (!m_departmentTree) return nullptr;

    DepartmentNode* node = m_departmentTree->findNodeById(navId);
    if (!node) return nullptr;

    Department* navModel = new Department;
    navModel->setScopeId(this->id());
    navModel->loadFromDepartmentNode(node);
    navModel->markSubdepartmentActive(m_currentNavigationId);

    // sharing m_inverseDepartments with getAltNavigation
    m_departmentModels.insert(navId, navModel);
    m_inverseDepartments.insert(navModel, navId);
    QObject::connect(navModel, &QObject::destroyed, this, &Scope::departmentModelDestroyed);

    return navModel;
}

unity::shell::scopes::NavigationInterface* Scope::getAltNavigation(QString const& navId)
{
    if (!m_altNavTree) return nullptr;

    DepartmentNode* node = m_altNavTree->findNodeById(navId);
    if (!node) return nullptr;

    Department* navModel = new Department;
    navModel->setScopeId(this->id());
    navModel->loadFromDepartmentNode(node);
    navModel->markSubdepartmentActive(m_currentAltNavigationId);

    // sharing m_inverseDepartments with getNavigation
    m_altNavModels.insert(navId, navModel);
    m_inverseDepartments.insert(navModel, navId);
    QObject::connect(navModel, &QObject::destroyed, this, &Scope::departmentModelDestroyed);

    return navModel;
}

QString Scope::buildQuery(QString const& scopeId, QString const& searchQuery, QString const& departmentId, QString const& primaryFilterId, QString const& primaryOptionId)
{
    scopes::CannedQuery q(scopeId.toStdString());
    q.set_query_string(searchQuery.toStdString());
    q.set_department_id(departmentId.toStdString());

    if (!primaryFilterId.isEmpty()) {
        scopes::FilterState filter_state;
        scopes::OptionSelectorFilter::update_state(filter_state, primaryFilterId.toStdString(), primaryOptionId.toStdString(), true);
        q.set_filter_state(filter_state);
    }

    return QString::fromStdString(q.to_uri());
}

void Scope::setNavigationState(QString const& navId, bool altNavigation)
{
    QString primaryFilterId;
    if (m_sortOrderFilter) {
        primaryFilterId = QString::fromStdString(m_sortOrderFilter->id());
    }
    if (!altNavigation) {
        // switch current department id
        performQuery(buildQuery(id(), m_searchQuery, navId, primaryFilterId, m_currentAltNavigationId));
    } else {
        // switch current primary filter
        performQuery(buildQuery(id(), m_searchQuery, m_currentNavigationId, primaryFilterId, navId));
    }
}

void Scope::departmentModelDestroyed(QObject* obj)
{
    scopes_ng::Department* navigation = reinterpret_cast<scopes_ng::Department*>(obj);

    auto it = m_inverseDepartments.find(navigation);
    if (it == m_inverseDepartments.end()) return;

    m_departmentModels.remove(it.value(), navigation);
    m_altNavModels.remove(it.value(), navigation);
    m_inverseDepartments.erase(it);
}

void Scope::performQuery(QString const& cannedQuery)
{
    try {
        scopes::CannedQuery q(scopes::CannedQuery::from_uri(cannedQuery.toStdString()));
        executeCannedQuery(q, true);
    } catch (...) {
        qWarning("Unable to parse canned query uri: %s", cannedQuery.toStdString().c_str());
    }
}

QString Scope::searchQuery() const
{
    return m_searchQuery;
}

QString Scope::noResultsHint() const
{
    return m_noResultsHint;
}

QString Scope::formFactor() const
{
    return m_formFactor;
}

bool Scope::isActive() const
{
    return m_isActive;
}

QString Scope::currentNavigationId() const
{
    return m_currentNavigationId;
}

bool Scope::hasNavigation() const
{
    return m_hasNavigation;
}

QString Scope::currentAltNavigationId() const
{
    return m_currentAltNavigationId;
}

bool Scope::hasAltNavigation() const
{
    return m_hasAltNavigation;
}

QVariantMap Scope::customizations() const
{
    return m_customizations;
}

void Scope::setSearchQuery(const QString& search_query)
{
    /* Checking for m_searchQuery.isNull() which returns true only when the string
       has never been set is necessary because when search_query is the empty
       string ("") and m_searchQuery is the null string,
       search_query != m_searchQuery is still true.
    */

    if (m_searchQuery.isNull() || search_query != m_searchQuery) {
        m_searchQuery = search_query;

        // FIXME: use a timeout
        invalidateResults();

        Q_EMIT searchQueryChanged();
    }
}

void Scope::setNoResultsHint(const QString& hint) {
    if (hint != m_noResultsHint) {
        m_noResultsHint = hint;
        Q_EMIT noResultsHintChanged();
    }
}

void Scope::setFormFactor(const QString& form_factor) {
    if (form_factor != m_formFactor) {
        m_formFactor = form_factor;
        // FIXME: force new search
        Q_EMIT formFactorChanged();
    }
}

void Scope::setActive(const bool active) {
    if (active != m_isActive) {
        m_isActive = active;
        Q_EMIT isActiveChanged();

        if (m_scopeMetadata && m_scopeMetadata->location_data_needed())
        {
            if (m_isActive)
            {
                m_locationService->activate();
            }
            else
            {
                m_locationService->deactivate();
            }
        }

        if (active && m_resultsDirty) {
            dispatchSearch();
        }
    }
}

void Scope::setFavorite(const bool value)
{
    Q_UNUSED(value);

    qWarning("Unimplemented: %s", __func__);
}

void Scope::activate(QVariant const& result_var)
{
    if (!result_var.canConvert<std::shared_ptr<scopes::Result>>()) {
        qWarning("Cannot activate, unable to convert %s to Result", result_var.typeName());
        return;
    }

    std::shared_ptr<scopes::Result> result = result_var.value<std::shared_ptr<scopes::Result>>();
    if (!result) {
        qWarning("activate(): received null result");
        return;
    }

    if (result->direct_activation()) {
        activateUri(QString::fromStdString(result->uri()));
    } else {
        try {
            cancelActivation();
            scopes::ActivationListenerBase::SPtr listener(new ActivationReceiver(this, result));
            m_activationController->setListener(listener);

            auto proxy = proxy_for_result(result);
            unity::scopes::ActionMetadata metadata(QLocale::system().name().toStdString(), m_formFactor.toStdString());
            scopes::QueryCtrlProxy controller = proxy->activate(*(result.get()), metadata, listener);
            m_activationController->setController(controller);
        } catch (std::exception& e) {
            qWarning("Caught an error from activate(): %s", e.what());
        } catch (...) {
            qWarning("Caught an error from activate()");
        }
    }
}

unity::shell::scopes::PreviewStackInterface* Scope::preview(QVariant const& result_var)
{
    if (!result_var.canConvert<std::shared_ptr<scopes::Result>>()) {
        qWarning("Cannot preview, unable to convert %s to Result", result_var.typeName());
        return nullptr;
    }

    scopes::Result::SPtr result = result_var.value<std::shared_ptr<scopes::Result>>();
    if (!result) {
        qWarning("preview(): received null result");
        return nullptr;
    }

    PreviewStack* stack = new PreviewStack(nullptr);
    stack->setAssociatedScope(this);
    stack->loadForResult(result);
    return stack;
}

void Scope::cancelActivation()
{
    m_activationController->invalidate();
}

void Scope::invalidateResults()
{
    if (m_isActive) {
        dispatchSearch();
    } else {
        // mark the results as dirty, so next setActive() re-sends the query
        if (!m_resultsDirty)
        {
            m_resultsDirty = true;
            resultsDirtyChanged();
        }
    }
}

void Scope::closeScope(unity::shell::scopes::ScopeInterface* scope)
{
    if (m_tempScopes.remove(scope)) {
        delete scope;
    }
}

bool Scope::resultsDirty() const {
    return m_resultsDirty;
}

void Scope::activateUri(QString const& uri)
{
    /* Tries various methods to trigger a sensible action for the given 'uri'.
       If it has no understanding of the given scheme it falls back on asking
       Qt to open the uri.
    */
    QUrl url(uri);
    if (url.scheme() == QLatin1String("scope")) {
        qDebug() << "Got scope URI" << uri;
        performQuery(uri);
    } else {
        qDebug() << "Trying to open" << uri;
        /* Try our luck */
        QDesktopServices::openUrl(url);
    }
}

} // namespace scopes_ng
