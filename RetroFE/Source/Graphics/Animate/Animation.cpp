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
#include <vector>

Animation::Animation() = default;

Animation::Animation(const Animation& copy) {
    // Shallow copy each TweenSet by copying the shared_ptr
    for (const auto& tweenSet : copy.animationVector_) {
        animationVector_.push_back(tweenSet);
    }
}

Animation& Animation::operator=(const Animation& other) {
    if (this != &other) { // Protection against self-assignment
        // Clear existing resources
        Clear();

        // Shallow copy each TweenSet by copying the shared_ptr
        for (const auto& tweenSet : other.animationVector_) {
            animationVector_.push_back(tweenSet);
        }
    }
    return *this;
}

Animation::~Animation()
{
    Clear();
}

void Animation::Push(std::shared_ptr<TweenSet> set) {
    animationVector_.push_back(set);
}

void Animation::Clear() {
    animationVector_.clear();
}

std::shared_ptr<TweenSet> Animation::tweenSet(unsigned int index) {
    if (index < animationVector_.size()) {
        return animationVector_[index]; // Return the shared pointer directly
    }
    else {
        // Handle the error or return nullptr if the index is out of bounds
        return nullptr;
    }
}

size_t Animation::size() const {
    return animationVector_.size();
}
