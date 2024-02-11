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

class TweenSet
{
public:
    TweenSet();
    TweenSet(const TweenSet& copy);
    TweenSet& operator=(const TweenSet& other);
    ~TweenSet();
    void push(std::unique_ptr<Tween> tween);
    void clear();
    Tween* getTween(unsigned int index) const;

    size_t size() const;

private:
    std::vector<std::unique_ptr<Tween>> set_;
};
