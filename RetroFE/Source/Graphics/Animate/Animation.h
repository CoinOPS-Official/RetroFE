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

#include "TweenSet.h"
#include <vector>

class Animation {
public:
    Animation();

    // C++20: Explicit noexcept move support allows std::vector to optimize 
    // reallocations and assignments.
    Animation(Animation&& other) noexcept = default;
    Animation& operator=(Animation&& other) noexcept = default;

    Animation(const Animation& copy) = default;
    Animation& operator=(const Animation& other) = default;
    ~Animation() = default;

    // Overload for lvalues (standard copy)
    void Push(const TweenSet& set);

    // C++20: Overload for rvalues (move - avoids vector allocations)
    void Push(TweenSet&& set);

    void Clear();

    // Rebuild a one-set runtime animation without releasing its tween storage.
    TweenSet& resetSingleSet();

    [[nodiscard]] TweenSet* tweenSet(unsigned int index);
    [[nodiscard]] size_t size() const noexcept;

private:
    std::vector<TweenSet> animationVector_;
};
