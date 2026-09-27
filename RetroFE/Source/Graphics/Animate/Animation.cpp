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
#include <utility> // for std::move

Animation::Animation() = default;

void Animation::Push(const TweenSet& set) {
    animationVector_.push_back(set); // Standard deep copy
}

void Animation::Push(TweenSet&& set) {
    // C++20: Move the set into the vector to "steal" its internal Tween vector
    animationVector_.emplace_back(std::move(set));
}

void Animation::Clear() {
    animationVector_.clear();
}

TweenSet& Animation::resetSingleSet() {
    if (animationVector_.empty()) {
        animationVector_.emplace_back();
    } else if (animationVector_.size() > 1) {
        animationVector_.resize(1);
    }
    animationVector_.front().clear();
    return animationVector_.front();
}

TweenSet* Animation::tweenSet(unsigned int index) {
    if (index < animationVector_.size()) {
        return &animationVector_[index];
    }
    return nullptr;
}

size_t Animation::size() const noexcept {
    return animationVector_.size();
}
