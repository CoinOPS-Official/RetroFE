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

#include "AnimationEvents.h"
#include <string>



AnimationEvents::AnimationEvents() = default;

AnimationEvents::~AnimationEvents()
{
    clear();
}

Animation *AnimationEvents::getAnimation(const std::string& tween)
{
    return getAnimation(tween, -1);
}

Animation* AnimationEvents::getAnimation(const std::string& tween, int index) {
    // Check if the tween exists, and if not, create a default Animation for this tween with index -1
    if (animationMap_.find(tween) == animationMap_.end()) {
        animationMap_[tween][-1] = new Animation();
    }

    // Use auto& to avoid copying and to directly refer to the inner map
    auto& indexMap = animationMap_[tween];

    // Check if the specific index exists for this tween; if not, use -1
    if (indexMap.find(index) == indexMap.end()) {
        index = -1;
    }

    return indexMap[index];
}

void AnimationEvents::setAnimation(const std::string& tween, int index, Animation* animation) {
    // Access the inner map directly to avoid multiple lookups
    auto& tweenAnimations = animationMap_[tween];

    // Find the existing animation at the given index
    auto existingAnimationIt = tweenAnimations.find(index);
    if (existingAnimationIt != tweenAnimations.end()) {
        // If there's an existing animation, delete it
        delete existingAnimationIt->second; // Delete the old Animation
        existingAnimationIt->second = animation; // Assign the new Animation
    }
    else {
        // Directly insert the new animation if there's no existing one at the index
        tweenAnimations[index] = animation;
    }
}


void AnimationEvents::clear()
{
    for (auto& [key, innerMap] : animationMap_) // This is the structured binding declaration
    {
        for (auto const& [innerKey, animation] : innerMap) // Another structured binding
        {
            delete animation;
        }
        innerMap.clear();
    }
    animationMap_.clear();
}