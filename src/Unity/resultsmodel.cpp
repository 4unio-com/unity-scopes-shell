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

// self
#include "resultsmodel.h"

// local
#include "utils.h"
#include "iconutils.h"

#include <map>
#include <QDebug>

namespace scopes_ng {

using namespace unity;

void SearchContext::reset()
{
    newResultsMap.clear();
    oldResultsMap.clear();
    lastResultIndex = 0;
}

ResultsModel::ResultsModel(QObject* parent)
 : unity::shell::scopes::ResultsModelInterface(parent)
 , m_maxAttributes(2)
 , m_purge(true)
{
    m_componentMapping.resize(RoleSocialActions + 1);
}

QString ResultsModel::categoryId() const
{
    return m_categoryId;
}

void ResultsModel::setCategoryId(QString const& id)
{
    if (m_categoryId != id) {
        m_categoryId = id;
        Q_EMIT categoryIdChanged();
    }
}

void ResultsModel::setComponentsMapping(QHash<QString, QString> const& mapping)
{
    QVector<std::string> newMapping(RoleSocialActions + 1);
    for (auto it = mapping.begin(); it != mapping.end(); ++it) {
        Roles field;
        const QString fieldName = it.key();
        if (fieldName == QLatin1String("title")) {
            field = RoleTitle;
        } else if (fieldName == QLatin1String("attributes")) {
            field = RoleAttributes;
        } else if (fieldName == QLatin1String("art")) {
            field = RoleArt;
        } else if (fieldName == QLatin1String("subtitle")) {
            field = RoleSubtitle;
        } else if (fieldName == QLatin1String("mascot")) {
            field = RoleMascot;
        } else if (fieldName == QLatin1String("emblem")) {
            field = RoleEmblem;
        } else if (fieldName == QLatin1String("summary")) {
            field = RoleSummary;
        } else if (fieldName == QLatin1String("background")) {
            field = RoleBackground;
        } else if (fieldName == QLatin1String("overlay-color")) {
            field = RoleOverlayColor;
        } else if (fieldName == QLatin1String("quick-preview-data")) {
            field = RoleQuickPreviewData;
        } else if (fieldName == QLatin1String("social-actions")) {
            field = RoleSocialActions;
        } else {
            qDebug() << "Unknown components field" << fieldName;
            break;
        }
        newMapping[field] = it.value().toStdString();
    }

    if (rowCount() > 0) {
        beginResetModel();
        m_componentMapping = newMapping;
        endResetModel();
    } else {
        m_componentMapping = newMapping;
    }
}

void ResultsModel::setMaxAtrributesCount(int count)
{
    m_maxAttributes = count;
}

void ResultsModel::addUpdateResults(QList<std::shared_ptr<unity::scopes::CategorisedResult>>& results)
{
    if (results.count() == 0) {
        return;
    }
 
    m_purge = false;

    // optimize for simple case when current view is initially empty - just add all the results
    if (m_results.count() == 0) {
        addResults(results);
        return;
    }

    const int oldCount = m_results.count();

    // update result -> index mappings with a subset of current result set, starting from lastResultIndex.
    m_search_ctx.newResultsMap.update(results, m_search_ctx.lastResultIndex);
  
#ifdef VERBOSE_MODEL_UPDATES
    qDebug() << "Last result index=" << m_search_ctx.lastResultIndex << "category" << m_categoryId;
#endif  
    
    int row = 0;
    // iterate over currently visible results, remove results which are no longer present in new set.
    // this needs to be done only once on first run of the new search, since in consecutive runs there
    // we will only by appending or moving.
    if (m_search_ctx.lastResultIndex == 0) {
        for (auto it = m_results.begin(); it != m_results.end(); ) {
            int newPos = m_search_ctx.newResultsMap.find(*it);
            bool haveNow = (newPos >= 0);
            if (!haveNow) {
                // delete row
                beginRemoveRows(QModelIndex(), row, row);
                it = m_results.erase(it);
                endRemoveRows();
            } else {
                ++it;
                ++row;
            }
        }
        // called only once on new search - it's cheaper to rebuild than to update
        // indices of all rows below removed row.
        m_search_ctx.oldResultsMap.rebuild(m_results);
    }
    
    // iterate over new results
    for (row = m_search_ctx.lastResultIndex; row<results.count(); ++row) {
        const int oldPos = m_search_ctx.oldResultsMap.find(results[row]);
        const bool hadBefore = (oldPos >= 0);
        if (hadBefore) {
            if (row != oldPos) {
                // move row
                beginMoveRows(QModelIndex(), oldPos, oldPos, QModelIndex(), row + (row > oldPos ? 1 : 0));
                m_results.move(oldPos, row);
                if (row < oldPos) {
                    m_search_ctx.oldResultsMap.updateIndices(m_results, row, oldPos);                    
                } else {
                    // This should never actually happen - as incoming results are iterated
                    // we should only be facing results which had greater position previously.
                    m_search_ctx.oldResultsMap.updateIndices(m_results, oldPos, row);
                }
                endMoveRows();
            }
        } else {
            // insert row
            beginInsertRows(QModelIndex(), row, row);
            m_results.insert(row, results[row]);
            m_search_ctx.oldResultsMap.updateIndices(m_results, row + 1, m_results.size());
            endInsertRows();
        }
    }

    m_search_ctx.lastResultIndex = results.count();

#ifdef VERBOSE_MODEL_UPDATES
    qDebug() << "Added #" << (m_results.count() - oldCount) << "results (called with" << results.count() << "), current results#=" << m_results.count();
#endif

    if (oldCount != m_results.count()) {
        Q_EMIT countChanged();
    }
}

void ResultsModel::addResults(QList<std::shared_ptr<unity::scopes::CategorisedResult>>& results)
{
#ifdef VERBOSE_MODEL_UPDATES
    qDebug() << "Adding #" << results.count() << "results to category" << m_categoryId;
#endif
    if (results.count() == 0) {
        return;
    }

    m_purge = false;

    m_search_ctx.newResultsMap = ResultsMap(results); // deduplicate results

    beginInsertRows(QModelIndex(), m_results.count(), m_results.count() + results.count() - 1);
    for (auto const& result: results) {
        m_results.append(result);
    }
    endInsertRows();

    m_search_ctx.oldResultsMap = m_search_ctx.newResultsMap;
    m_search_ctx.lastResultIndex = m_results.count();

    Q_EMIT countChanged();
}

void ResultsModel::clearResults()
{
#ifdef VERBOSE_MODEL_UPDATES
    qDebug() << "ResultsModel::clearResults(), category" << m_categoryId;
#endif
    
    if (m_results.count() == 0) return;

    beginRemoveRows(QModelIndex(), 0, m_results.count() - 1);
    m_results.clear();
    endRemoveRows();

    m_search_ctx.reset();

    Q_EMIT countChanged();
}

int ResultsModel::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);

    return m_results.count();
}

int ResultsModel::count() const
{
    return m_results.count();
}

QVariant
ResultsModel::componentValue(scopes::Result const* result, Roles field) const
{
    std::string const& realFieldName = m_componentMapping[field];
    if (realFieldName.empty())
        return QVariant();
    try {
        scopes::Variant const& v = result->value(realFieldName);
        return scopeVariantToQVariant(v);
    } catch (...) {
        // value() throws if realFieldName is empty or the result
        // doesn't have a value for it
        return QVariant();
    }
}

QVariant
ResultsModel::attributesValue(scopes::Result const* result) const
{
    try {
        std::string const& realFieldName = m_componentMapping[RoleAttributes];
        scopes::Variant const& v = result->value(realFieldName);
        if (v.which() != scopes::Variant::Type::Array) {
            return QVariant();
        }

        QVariantList attributes;
        scopes::VariantArray arr(v.get_array());
        for (size_t i = 0; i < arr.size(); i++) {
            if (arr[i].which() != scopes::Variant::Type::Dict) {
                continue;
            }
            QVariantMap attribute(scopeVariantToQVariant(arr[i]).toMap());
            attributes << QVariant(attribute);
            // we'll limit the number of attributes
            if (attributes.size() >= m_maxAttributes) {
                break;
            }
        }

        return attributes;
    } catch (...) {
        // value() throws if realFieldName is empty or the result
        // doesn't have a value for it
        return QVariant();
    }
}

QHash<int, QByteArray> ResultsModel::roleNames() const
{
    QHash<int, QByteArray> roles(unity::shell::scopes::ResultsModelInterface::roleNames());
    roles.insert(ExtraRoles::RoleScopeId, "scopeId");

    return roles;
}

void ResultsModel::updateResult(scopes::Result const& result, scopes::Result const& updatedResult)
{
    for (int i = 0; i<m_results.size(); i++)
    {
        auto const res = m_results[i];
        if (result.uri() == res->uri() && result.serialize() == res->serialize())
        {
            qDebug() << "Updated result with uri '" << QString::fromStdString(res->uri()) << "'";
            m_results[i] = std::make_shared<scopes::Result>(updatedResult);
            auto const idx = index(i, 0);
            Q_EMIT dataChanged(idx, idx);
            return;
        }
    }
    qWarning() << "ResultsModel::updateResult - failed to find result with uri '"
        << QString::fromStdString(result.uri())
        << "', category '" << categoryId() << "'";
}

QVariant
ResultsModel::data(const QModelIndex& index, int role) const
{
    const int row = index.row();
    if (row >= m_results.size())
    {
        qWarning() << "ResultsModel::data - invalid index" << row << "size"
                << m_results.size();
        return QVariant();
    }

    scopes::Result* result = m_results.at(row).get();

    switch (role) {
        case RoleUri:
            return QString::fromStdString(result->uri());
        case RoleCategoryId:
            return categoryId();
        case RoleDndUri:
            return QString::fromStdString(result->dnd_uri());
        case RoleResult:
            return QVariant::fromValue(std::static_pointer_cast<unity::scopes::Result>(m_results.at(row)));
        case RoleArt: {
            QString image(componentValue(result, RoleArt).toString());
            if (image.isEmpty()) {
                QString uri(QString::fromStdString(result->uri()));
                // FIXME: figure out a better way and get rid of this, it's an awful hack
                QVariantHash result_meta;
                if (result->contains("artist") && result->contains("album")) {
                    result_meta[QStringLiteral("artist")] = scopeVariantToQVariant(result->value("artist"));
                    result_meta[QStringLiteral("album")] = scopeVariantToQVariant(result->value("album"));
                }
                QString thumbnailerUri(uriToThumbnailerProviderString(uri, result_meta));
                if (!thumbnailerUri.isNull()) {
                    return thumbnailerUri;
                }
            }
            return image;
        }
        case RoleTitle:
        case RoleSubtitle:
        case RoleMascot:
        case RoleEmblem:
        case RoleSummary:
        case RoleOverlayColor:
        case RoleQuickPreviewData:
        case RoleSocialActions:
            return componentValue(result, Roles(role));
        case RoleAttributes:
            return attributesValue(result);
        case RoleBackground: {
            QVariant backgroundVariant(componentValue(result, RoleBackground));
            if (backgroundVariant.isNull()) {
                return backgroundVariant;
            }
            return backgroundUriToVariant(backgroundVariant.toString());
        }
        case RoleScopeId:
            if (result->uri().compare(0, 8, "scope://") == 0) {
                try {
                    scopes::CannedQuery q(scopes::CannedQuery::from_uri(result->uri()));
                    return QString::fromStdString(q.scope_id());
                } catch (...) {
                    // silently ignore and return "undefined"
                }
            }
            return QVariant();
        default:
            return QVariant();
    }
}

void ResultsModel::markNewSearch()
{
    m_purge = true;
    m_search_ctx.lastResultIndex = 0;
    m_search_ctx.newResultsMap.clear();
}

bool ResultsModel::needsPurging() const
{
    return m_purge;
}

} // namespace scopes_ng
