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

AnimationEvents::AnimationEvents()
{
}

AnimationEvents::AnimationEvents(AnimationEvents& copy)
{
    for (const auto& outerPair : copy.animationMap_)
    {
        for (const auto& innerPair : outerPair.second)
        {
            animationMap_[outerPair.first][innerPair.first] = new Animation(*innerPair.second);
        }
    }
}

AnimationEvents::~AnimationEvents()
{
    clear();
}

Animation* AnimationEvents::getAnimation(const std::string& tween)
{
    return getAnimation(tween, -1);
}

Animation* AnimationEvents::getAnimation(const std::string& tween, size_t index)
{
    auto outerIt = animationMap_.find(tween);

    // If tween not found, create a new Animation and return it
    if (outerIt == animationMap_.end())
    {
        animationMap_[tween][-1] = new Animation();
        return animationMap_[tween][-1];
    }

    auto& innerMap = outerIt->second;
    auto innerIt = innerMap.find(index);

    // If index not found, return the Animation at -1; 
    // if -1 also doesn't exist, create it
    if (innerIt == innerMap.end())
    {
        if (innerMap.find(-1) == innerMap.end())
        {
            innerMap[-1] = new Animation();
        }
        return innerMap[-1];
    }

    return innerIt->second;
}

void AnimationEvents::setAnimation(const std::string& tween, int index, Animation* animation)
{
    auto outerIt = animationMap_.find(tween);
    if (outerIt != animationMap_.end())
    {
        auto& innerMap = outerIt->second;
        auto innerIt = innerMap.find(index);
        if (innerIt != innerMap.end())
        {
            delete innerIt->second;
            innerMap.erase(innerIt);
        }
    }
    animationMap_[tween][index] = animation;
}

void AnimationEvents::clear()
{
    for (auto& outerPair : animationMap_)
    {
        for (auto& innerPair : outerPair.second)
        {
            delete innerPair.second;
        }
        outerPair.second.clear();
    }
    animationMap_.clear();
}
