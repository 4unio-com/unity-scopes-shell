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

#include <QObject>
#include <QTest>
#include <QJsonValue>
#include <QJsonObject>
#include <QThread>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QVariantList>

#include <scopes.h>
#include <scope.h>
#include <categories.h>
#include <overviewresults.h>
#include <previewmodel.h>
#include <previewwidgetmodel.h>

#include <scope-harness/registry/pre-existing-registry.h>
#include <scope-harness/test-utils.h>

using namespace scopes_ng;
using namespace unity::scopeharness;
using namespace unity::scopeharness::registry;

class OverviewTest : public QObject
{
    Q_OBJECT
private:
    QScopedPointer<Scopes> m_scopes;
    Scope::Ptr m_scope;
    Registry::UPtr m_registry;

private Q_SLOTS:
    void initTestCase()
    {
        m_registry.reset(new PreExistingRegistry(TEST_RUNTIME_CONFIG));
        m_registry->start();
    }

    void cleanupTestCase()
    {
        m_registry.reset();
    }

    void init()
    {
        QStringList favs;
        favs << "scope://mock-scope-departments" << "scope://mock-scope-double-nav";
        TestUtils::setFavouriteScopes(favs);

        m_scopes.reset(new Scopes(nullptr));
        // no scopes on startup
        QCOMPARE(m_scopes->rowCount(), 0);
        QCOMPARE(m_scopes->loaded(), false);
        QSignalSpy spy(m_scopes.data(), SIGNAL(loadedChanged()));
        // wait till the registry spawns
        QVERIFY(spy.wait());
        QCOMPARE(m_scopes->loaded(), true);

        // get scope proxy
        m_scope = m_scopes->overviewScopeSPtr();
        QVERIFY(bool(m_scope));
    }

    void cleanup()
    {
        m_scopes.reset();
        m_scope.reset();
    }

    void testScopeProperties()
    {
        QCOMPARE(m_scope->id(), QString("scopes"));
    }

    void testSurfacingQuery()
    {
        // ensure categories have > 0 rows
        auto categories = m_scope->categories();
        QVERIFY(categories->rowCount() > 0);
        QCOMPARE(categories->data(categories->index(0), Categories::Roles::RoleCategoryId), QVariant(QString("favorites")));
        QCOMPARE(categories->data(categories->index(1), Categories::Roles::RoleCategoryId), QVariant(QString("other")));

        QVariant results_var = categories->data(categories->index(0), Categories::Roles::RoleResults);
        QVERIFY(results_var.canConvert<OverviewResultsModel*>());
        OverviewResultsModel* results = results_var.value<OverviewResultsModel*>();
        QVERIFY(results->rowCount() == 2);
    }
};

QTEST_GUILESS_MAIN(OverviewTest)
#include <overviewtest.moc>
