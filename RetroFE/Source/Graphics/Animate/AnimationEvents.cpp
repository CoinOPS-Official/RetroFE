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

AnimationEvents::AnimationEvents(AnimationEvents& copy)
{
    for (const auto& [outerKey, outerValue] : copy.animationMap_)
    {
        auto& innerMap = animationMap_[outerKey];
        for (const auto& [innerKey, innerValue] : outerValue)
        {
            innerMap[innerKey] = new Animation(*innerValue);
        }
    }
}


AnimationEvents::~AnimationEvents()
{
    clear();
}

Animation *AnimationEvents::getAnimation(const std::string& tween)
{
    return getAnimation(tween, -1);
}

Animation *AnimationEvents::getAnimation(const std::string& tween, int index)
{
    if(animationMap_[tween].find(-1) == animationMap_[tween].end())
    {
        animationMap_[tween][-1] = new Animation();
    }

    if(animationMap_[tween].find(index) == animationMap_[tween].end())
    {
        index = -1;
    }

    return animationMap_[tween][index];
}


void AnimationEvents::setAnimation(const std::string& tween, int index, Animation *animation)
{
    if(animationMap_[tween].find(index) != animationMap_[tween].end())
    {
        delete animationMap_[tween][index];
    }
    animationMap_[tween][index] = animation;
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