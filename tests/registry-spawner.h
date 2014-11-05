/*
 * Copyright (C) 2013-2014 Canonical, Ltd.
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
 *
 * Authors:
 *  Michal Hruby <michal.hruby@canonical.com>
 */

#ifndef REGISTRY_SPAWNER_H
#define REGISTRY_SPAWNER_H

#include <QProcess>
#include <QTemporaryDir>
#include <QScopedPointer>

class Q_DECL_EXPORT RegistrySpawner
{
public:
    RegistrySpawner();

    ~RegistrySpawner();

private:
    QScopedPointer<QProcess> m_registry;

    QTemporaryDir m_tempDir;
};

#endif // REGISTRY_SPAWNER_H
