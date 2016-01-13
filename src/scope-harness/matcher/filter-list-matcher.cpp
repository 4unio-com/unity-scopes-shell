/*
 * Copyright (C) 2016 Canonical, Ltd.
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of version 3 of the GNU Lesser General Public License as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Pawel Stolowski <pawel.stolowski@canonical.com>
 */

#include <scope-harness/matcher/filter-list-matcher.h>
#include <scope-harness/matcher/filter-matcher.h>

using namespace std;

namespace unity
{
namespace scopeharness
{
namespace matcher
{

struct FilterListMatcher::_Priv
{
    int x;
};

FilterListMatcher::FilterListMatcher()
: p(new _Priv)
{
}

FilterListMatcher& FilterListMatcher::mode(FilterListMatcher::Mode mode)
{
    return *this;
}

FilterListMatcher& FilterListMatcher::filter(const FilterMatcher& filterMatcher)
{
    return *this;
}

FilterListMatcher& FilterListMatcher::filter(FilterMatcher&& filterMatcher)
{
    return *this;
}

FilterListMatcher& FilterListMatcher::hasAtLeast(std::size_t minimum)
{
    return *this;
}

FilterListMatcher& FilterListMatcher::hasExactly(std::size_t amount)
{
    return *this;
}

MatchResult FilterListMatcher::match(const view::FiltersView::SPtr& filterList) const
{
    MatchResult matchResult;
    return matchResult;
}

}
}
}
