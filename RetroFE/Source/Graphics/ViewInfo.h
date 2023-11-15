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

#include "Animate/TweenTypes.h"
#include <string>
#include <map>
#include <cfloat>

class Font;

class ViewInfo
{
public:

    ViewInfo();
    virtual ~ViewInfo();

    float XRelativeToOrigin() const;
    float YRelativeToOrigin() const;

    float ScaledHeight() const;
    float ScaledWidth() const;

    static const int AlignCenter = -1;
    static const int AlignLeft = -2;
    static const int AlignTop = -3;
    static const int AlignRight = -4;
    static const int AlignBottom = -5;

    float        X{ 0 };
    float        Y{ 0 };
    float        XOrigin{ 0 };
    float        YOrigin{ 0 };
    float        XOffset{ 0 };
    float        YOffset{ 0 };
    float        Width{ -1 };
    float        MinWidth{ 0 };
    float        MaxWidth{ FLT_MAX };
    float        Height{ -1 };
    float        MinHeight{ 0 };
    float        MaxHeight{ FLT_MAX };
    float        ImageWidth{ 0 };
    float        ImageHeight{ 0 };
    float        FontSize{ -1 };
    Font* font{ nullptr };
    float        Angle{ 0 };
    float        Alpha{ 1 };
    unsigned int Layer{ 0 };
    unsigned int Layout{ 0 };
    float        BackgroundRed{ 0 };
    float        BackgroundGreen{ 0 };
    float        BackgroundBlue{ 0 };
    float        BackgroundAlpha{ 0 };
    std::string  Reflection{ "" };
    unsigned int ReflectionDistance{ 0 };
    float        ReflectionScale{ .25 };
    float        ReflectionAlpha{ 1 };
    float        ContainerX{ 0 };
    float        ContainerY{ 0 };
    float        ContainerWidth{ -1 };
    float        ContainerHeight{ -1 };
    int          Monitor{ 0 };
    float        Volume{ 0 };
    bool         Restart{ false };
    bool         Additive{ false };
    bool         PauseOnScroll{ true };

private:
    float AbsoluteHeight() const;
    float AbsoluteWidth() const;
};
