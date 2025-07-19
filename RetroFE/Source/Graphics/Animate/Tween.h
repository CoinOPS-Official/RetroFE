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

#include "TweenTypes.h"
#include <string>
#include <unordered_map>

class ViewInfo;

class Tween {
public:

    Tween(TweenProperty property, TweenAlgorithm type, float start, float end, float duration, const std::string& playlistFilter = "");

    // Animate using high-precision elapsed time
    float animate(double elapsedTime) const;
    float animate(double elapsedTime, float startValue) const;

    // Core animation logic using floats for performance
    static float animateSingle(TweenAlgorithm type, float start, float end, float duration, float elapsedTime);

    static TweenAlgorithm getTweenType(const std::string& name);
    static bool getTweenProperty(const std::string& name, TweenProperty& property);

    TweenProperty property;
    float  duration;
    bool   startDefined{ true };
    std::string playlistFilter;

private:
    static float easeInQuadratic(float elapsedTime, float duration, float b, float c);
    static float easeOutQuadratic(float elapsedTime, float duration, float b, float c);
    static float easeInOutQuadratic(float elapsedTime, float duration, float b, float c);
    static float easeInCubic(float elapsedTime, float duration, float b, float c);
    static float easeOutCubic(float elapsedTime, float duration, float b, float c);
    static float easeInOutCubic(float elapsedTime, float duration, float b, float c);
    static float easeInQuartic(float elapsedTime, float duration, float b, float c);
    static float easeOutQuartic(float elapsedTime, float duration, float b, float c);
    static float easeInOutQuartic(float elapsedTime, float duration, float b, float c);
    static float easeInQuintic(float elapsedTime, float duration, float b, float c);
    static float easeOutQuintic(float elapsedTime, float duration, float b, float c);
    static float easeInOutQuintic(float elapsedTime, float duration, float b, float c);
    static float easeInSine(float elapsedTime, float duration, float b, float c);
    static float easeOutSine(float elapsedTime, float duration, float b, float c);
    static float easeInOutSine(float elapsedTime, float duration, float b, float c);
    static float easeInExponential(float elapsedTime, float duration, float b, float c);
    static float easeOutExponential(float elapsedTime, float duration, float b, float c);
    static float easeInOutExponential(float elapsedTime, float duration, float b, float c);
    static float easeInCircular(float elapsedTime, float duration, float b, float c);
    static float easeOutCircular(float elapsedTime, float duration, float b, float c);
    static float easeInOutCircular(float elapsedTime, float duration, float b, float c);
    static float linear(float elapsedTime, float duration, float b, float c);

    static std::unordered_map<std::string, TweenAlgorithm> tweenTypeMap_;
    static std::unordered_map<std::string, TweenProperty> tweenPropertyMap_;

    TweenAlgorithm type;
    float start;
    float end;
};