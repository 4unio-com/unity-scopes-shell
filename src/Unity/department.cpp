/*
 * Copyright (C) 2014 Canonical, Ltd.
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

#include "department.h"

#include <unity/scopes/OptionSelectorFilter.h>

#include <QDebug>

namespace scopes_ng
{

using namespace unity;

Department::Department(QObject* parent) :
    unity::shell::scopes::NavigationInterface(parent),
    m_loaded(false), m_isRoot(false), m_hidden(false), m_isFilter(false)
{
}

void Department::setScopeId(QString const& scopeId)
{
    m_scopeId = scopeId;
}

void Department::loadFromDepartmentNode(DepartmentNode* treeNode)
{
    if (treeNode == nullptr) {
        qWarning("Tried to set null DepartmentNode!");
        return;
    }
    m_navigationId = treeNode->id();
    m_filterId = treeNode->filterId();
    m_label = treeNode->label();
    m_allLabel = treeNode->allLabel();
    m_loaded = treeNode->isLeaf() || treeNode->childCount() > 0;
    m_isRoot = treeNode->isRoot();
    m_hidden = treeNode->hidden();
    m_isFilter = treeNode->isFilter();

    DepartmentNode* parentNode = treeNode->parent();
    m_parentNavigationId = parentNode ? parentNode->id() : QLatin1String("");
    m_parentLabel = parentNode ? parentNode->label() : QLatin1String("");

    beginResetModel();

    m_subdepartments.clear();
    Q_FOREACH (DepartmentNode* node, treeNode->childNodes()) {
        QSharedPointer<SubdepartmentData> subdept(new SubdepartmentData);
        subdept->id = node->id();
        subdept->label = node->label();
        subdept->allLabel = node->allLabel();
        subdept->hasChildren = node->hasSubdepartments();
        subdept->isActive = false;
        m_subdepartments.append(subdept);
    }

    endResetModel();

    Q_EMIT navigationIdChanged();
    Q_EMIT labelChanged();
    Q_EMIT allLabelChanged();
    Q_EMIT parentNavigationIdChanged();
    Q_EMIT parentLabelChanged();
    Q_EMIT loadedChanged();
    Q_EMIT countChanged();
    Q_EMIT isRootChanged();
    Q_EMIT hiddenChanged();
}

void Department::markSubdepartmentActive(QString const& subdepartmentId)
{
    int idx = -1;
    bool isActiveReset = false;
    for (int i = 0; i < m_subdepartments.count(); i++) {
        if (m_subdepartments[i]->id == subdepartmentId) {
            m_subdepartments[i]->isActive = true;
            idx = i;
        } else if (m_subdepartments[i]->isActive) {
            // only one department can be active
            m_subdepartments[i]->isActive = false;
            isActiveReset = true;
        }
    }

    if (idx < 0) return;

    QVector<int> roles;
    roles.append(Roles::RoleIsActive);

    QModelIndex startIndex(index(isActiveReset ? 0 : idx));
    QModelIndex endIndex(index(isActiveReset ? m_subdepartments.count() - 1 : idx));
    dataChanged(startIndex, endIndex, roles);
}

QVariant Department::data(const QModelIndex& index, int role) const
{
    int row = index.row();
    if (row >= m_subdepartments.size())
    {
        qWarning() << "Department::data - invalid index" << row << "size"
                << m_subdepartments.size();
        return QVariant();
    }

    SubdepartmentData* data = m_subdepartments[index.row()].data();
    switch (role) {
        case RoleNavigationId: return data->id;
        case RoleLabel: return data->label;
        case RoleAllLabel: return data->allLabel;
        case RoleHasChildren: return data->hasChildren;
        case RoleIsActive: return data->isActive;
        default: return QVariant();
    }
}

QSharedPointer<SubdepartmentData> Department::findSubdepartment(const QString &id) const
{
    for (auto sub: m_subdepartments) {
        if (sub->id == id) {
            return sub;
        }
    }
    return QSharedPointer<SubdepartmentData>();
}

int Department::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return m_subdepartments.size();
}

QString Department::navigationId() const
{
    return m_navigationId;
}

QString Department::label() const
{
    return m_label;
}

QString Department::allLabel() const
{
    return m_allLabel;
}

QString Department::parentNavigationId() const
{
    return m_parentNavigationId;
}

QString Department::parentLabel() const
{
    return m_parentLabel;
}

bool Department::loaded() const
{
    return m_loaded;
}

bool Department::isRoot() const
{
    return m_isRoot;
}

bool Department::hidden() const
{
    return m_hidden;
}

int Department::count() const
{
    return rowCount();
}

} // namespace scopes_ng
