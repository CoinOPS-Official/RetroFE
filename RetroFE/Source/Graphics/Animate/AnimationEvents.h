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
#include "Animation.h"
#include <string>
#include <vector>
#include <map>
#include <memory> // Include memory for std::shared_ptr

class AnimationEvents {
public:
    AnimationEvents();
    ~AnimationEvents();

    std::shared_ptr<Animation> getAnimation(const std::string& tween);
    std::shared_ptr<Animation> getAnimation(const std::string& tween, int index);
    void setAnimation(const std::string& tween, int index, std::shared_ptr<Animation> animation);
    void clear();

    // Getter for animationMap_
    const std::map<std::string, std::map<int, std::shared_ptr<Animation>>>& getAnimationMap() const;

private:
    std::map<std::string, std::map<int, std::shared_ptr<Animation>>> animationMap_;
};
