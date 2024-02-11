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

#include "Animation.h"
#include <string>
#include <memory>

Animation::Animation() = default;

Animation::Animation(const Animation& copy) {
    for (const auto& tweenSet : copy.animationVector_) {
        // Make a deep copy of each TweenSet and store it in a std::unique_ptr
        animationVector_.push_back(std::make_unique<TweenSet>(*tweenSet));
    }
}

Animation& Animation::operator=(const Animation& other) {
    if (this != &other) { // Protection against self-assignment
        // Clear existing resources
        Clear();

        // Deep copy each TweenSet
        for (const auto& tweenSet : other.animationVector_) {
            animationVector_.push_back(std::make_unique<TweenSet>(*tweenSet));
        }
    }
    return *this;
}

Animation::~Animation()
{
    Clear();
}

void Animation::Push(std::unique_ptr<TweenSet> set) {
    animationVector_.push_back(std::move(set));
}

void Animation::Clear() {
    animationVector_.clear();
}



TweenSet* Animation::tweenSet(unsigned int index) {
    if (index < animationVector_.size()) {
        return animationVector_[index].get(); // Use get() to obtain the raw pointer
    }
    else {
        // Handle the error or return nullptr if the index is out of bounds
        return nullptr;
    }
}


size_t Animation::size() const
{
    return animationVector_.size();
}
