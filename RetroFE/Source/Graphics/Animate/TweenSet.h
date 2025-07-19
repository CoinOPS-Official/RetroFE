/* This file is part of RetroFE.
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RetroFE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RetroFE.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include "Tween.h"
#include <vector>
#include <memory>
#include <map>

#include "TweenPool.h" // Add this include

 // This custom deleter doesn't delete memory, it returns the object to our pool.
struct TweenPoolDeleter {
    void operator()(Tween* p) const {
        if (p) {
            TweenPool::getInstance().release(p);
        }
    }
};

// Define a convenient type alias for a unique_ptr that uses our custom deleter.
using PooledTweenPtr = std::unique_ptr<Tween, TweenPoolDeleter>;

class TweenSet
{
public:
    TweenSet();
    ~TweenSet();

    // --- FORBID COPYING ---
// By deleting these, the compiler will give an error if anything
// tries to copy a TweenSet, which is now the safe behavior.
    TweenSet(const TweenSet& other) = delete;
    TweenSet& operator=(const TweenSet& other) = delete;

    // We also forbid moving for simplicity, as we pass TweenSets by pointer (shared_ptr)
    TweenSet(TweenSet&& other) = delete;
    TweenSet& operator=(TweenSet&& other) = delete;

    void push(PooledTweenPtr tween);
    void clear();
    Tween* getTween(TweenProperty property) const;
    Tween* getTween(unsigned int index) const;

    size_t size() const;

private:
    std::map<TweenProperty, PooledTweenPtr> set_;

    // This vector still stores non-owning pointers
    std::vector<Tween*> ordered_tweens_;
};
