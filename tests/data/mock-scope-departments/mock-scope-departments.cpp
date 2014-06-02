/*
 * Copyright (C) 2014 Canonical Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Pete Woods <pete.woods@canonical.com>
 */

#include <unity/scopes/CategorisedResult.h>
#include <unity/scopes/ScopeBase.h>
#include <unity/scopes/SearchReply.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <sstream>

#define EXPORT __attribute__ ((visibility ("default")))

using namespace std;
using namespace unity::scopes;

class MyQuery : public SearchQueryBase
{
public:
    MyQuery(CannedQuery const& query) :
            query_(query), department_id_(query.department_id())
    {
    }

    ~MyQuery()
    {
    }

    virtual void cancelled() override
    {
    }

    virtual void run(SearchReplyProxy const& reply) override
    {
        Department::SPtr child_dep;
        Department::SPtr root_dep;
        Department::SPtr active_dep;
        root_dep = Department::create("", query_, "All departments");

        child_dep = Department::create("books", query_, "Books");
        child_dep->set_has_subdepartments();
        root_dep->add_subdepartment(child_dep);

        if (department_id_.compare(0, 5, "books") == 0)
        {
            active_dep = child_dep;
            child_dep = Department::create("books-kindle", query_, "Kindle Books");
            active_dep->add_subdepartment(child_dep);

            child_dep = Department::create("books-study", query_, "Books for Study");
            active_dep->add_subdepartment(child_dep);

            child_dep = Department::create("books-audio", query_, "Audiobooks");
            active_dep->add_subdepartment(child_dep);

            // and this is the only leaf department for which the scope provides good data
            if (department_id_ == "books-audio") active_dep = child_dep;
        }

        child_dep = Department::create("movies", query_, "Movies, TV, Music");
        child_dep->set_has_subdepartments();
        root_dep->add_subdepartment(child_dep);

        child_dep = Department::create("electronics", query_, "Electronics");
        child_dep->set_has_subdepartments();
        root_dep->add_subdepartment(child_dep);

        child_dep = Department::create("home", query_, "Home, Garden & DIY");
        child_dep->set_has_subdepartments();
        root_dep->add_subdepartment(child_dep);

        if (department_id_.compare(0, 4, "home") == 0)
        {
            active_dep = child_dep;
            child_dep = Department::create("home-garden", query_, "Garden & Outdoors");
            active_dep->add_subdepartment(child_dep);

            child_dep = Department::create("home-furniture", query_, "Homeware & Furniture");
            active_dep->add_subdepartment(child_dep);

            child_dep = Department::create("home-kitchen", query_, "Kitchen & Dining");
            active_dep->add_subdepartment(child_dep);
        }

        child_dep = Department::create("toys", query_, "Toys, Children & Baby");
        child_dep->set_has_subdepartments();
        root_dep->add_subdepartment(child_dep);

        if (department_id_.compare(0, 4, "toys") == 0)
        {
            active_dep = child_dep;
            child_dep = Department::create("toys-games", query_, "Toys & Games");
            active_dep->add_subdepartment(child_dep);

            child_dep = Department::create("toys-baby", query_, "Baby");
            active_dep->add_subdepartment(child_dep);
        }

        if (!active_dep)
        {
            active_dep = root_dep;
        }

        reply->register_departments(root_dep, active_dep);

        auto cat1 = reply->register_category("cat1", "Category 1", "");
        CategorisedResult res1(cat1);
        res1.set_uri("test:uri");
        res1.set_title("result for: \"" + query_.query_string() + "\"");
        reply->push(res1);
    }

protected:
    CannedQuery query_;
    string department_id_;
};

class MyScope : public ScopeBase
{
public:
    MyScope()
    {
    }

    virtual int start(string const&, RegistryProxy const&) override
    {
        return VERSION;
    }

    virtual void stop() override {
    }

    virtual SearchQueryBase::UPtr search(CannedQuery const& q, SearchMetadata const&) override
    {
        return SearchQueryBase::UPtr(new MyQuery(q));
    }

    virtual PreviewQueryBase::UPtr preview(Result const&, ActionMetadata const&) override
    {
        return nullptr;
    }
};

extern "C"
{

    EXPORT
    unity::scopes::ScopeBase*
    // cppcheck-suppress unusedFunction
    UNITY_SCOPE_CREATE_FUNCTION()
    {
        return new MyScope;
    }

    EXPORT
    void
    // cppcheck-suppress unusedFunction
    UNITY_SCOPE_DESTROY_FUNCTION(unity::scopes::ScopeBase* scope_base)
    {
        delete scope_base;
    }

}
