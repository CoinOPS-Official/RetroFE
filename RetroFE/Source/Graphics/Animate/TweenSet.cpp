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
#include "TweenSet.h"

TweenSet::TweenSet() = default;

TweenSet::~TweenSet() {
    clear();
}

// This is the implementation for the corrected push method.
void TweenSet::push(PooledTweenPtr tween) {
    // Get the property and raw pointer before std::move invalidates the ptr
    TweenProperty property = tween->property;
    Tween* raw_ptr = tween.get();

    // Store the raw (non-owning) pointer for indexed access
    ordered_tweens_.push_back(raw_ptr);

    // Move the pooled (owning) pointer into the map
    set_[property] = std::move(tween);
}

void TweenSet::clear() {
    // When the map is cleared, the unique_ptrs are destroyed.
    // Our custom deleter is called for each one, returning the Tweens to the pool.
    set_.clear();

    // The vector just holds non-owning pointers, so we just clear the vector.
    ordered_tweens_.clear();
}

// Implementation for the NEW fast lookup by property
Tween* TweenSet::getTween(TweenProperty property) const {
    auto it = set_.find(property);
    if (it != set_.end()) {
        return it->second.get();
    }
    return nullptr;
}

// Corrected implementation for the OLD lookup by index
Tween* TweenSet::getTween(unsigned int index) const {
    if (index < ordered_tweens_.size()) {
        return ordered_tweens_[index];
    }
    return nullptr;
}

size_t TweenSet::size() const {
    // The size of both containers should always be identical.
    return set_.size();
}