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
#include <memory>
#include <map>

AnimationEvents::AnimationEvents() = default;

AnimationEvents::~AnimationEvents()
{
    clear();
}

std::shared_ptr<Animation> AnimationEvents::getAnimation(const std::string& tween)
{
    return getAnimation(tween, -1);
}

std::shared_ptr<Animation> AnimationEvents::getAnimation(const std::string& tween, int index)
{
    if (animationMap_.find(tween) == animationMap_.end())
        animationMap_[tween][-1] = std::make_shared<Animation>();

    if (animationMap_[tween].find(index) == animationMap_[tween].end())
        index = -1;

    return animationMap_[tween][index];
}

void AnimationEvents::setAnimation(const std::string& tween, int index, std::shared_ptr<Animation> animation)
{
    animationMap_[tween][index] = std::move(animation);
}

void AnimationEvents::clear()
{
    animationMap_.clear(); // std::unique_ptr will automatically clean up
}

// Getter for animationMap_
const std::map<std::string, std::map<int, std::shared_ptr<Animation>>>& AnimationEvents::getAnimationMap() const {
    return animationMap_;
}