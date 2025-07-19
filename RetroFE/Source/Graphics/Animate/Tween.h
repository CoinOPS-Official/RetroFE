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
#include <optional>

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
    static std::optional<TweenProperty> getTweenProperty(const std::string& name);

    void reinit(TweenProperty prop, TweenAlgorithm alg, float newStart, float newEnd, float newDuration, const std::string& playlist);

    TweenProperty property;
    float  duration;
    bool   startDefined{ true };
    std::string playlistFilter;

private:
    // Easing functions use a normalized progress value for calculation.
    // p: progress (0.0 to 1.0), b: beginning value, c: change in value (end - start).
    static float linear(float p, float b, float c);
    static float easeInQuadratic(float p, float b, float c);
    static float easeOutQuadratic(float p, float b, float c);
    static float easeInOutQuadratic(float p, float b, float c);
    static float easeInCubic(float p, float b, float c);
    static float easeOutCubic(float p, float b, float c);
    static float easeInOutCubic(float p, float b, float c);
    static float easeInQuartic(float p, float b, float c);
    static float easeOutQuartic(float p, float b, float c);
    static float easeInOutQuartic(float p, float b, float c);
    static float easeInQuintic(float p, float b, float c);
    static float easeOutQuintic(float p, float b, float c);
    static float easeInOutQuintic(float p, float b, float c);
    static float easeInSine(float p, float b, float c);
    static float easeOutSine(float p, float b, float c);
    static float easeInOutSine(float p, float b, float c);
    static float easeInExponential(float p, float b, float c);
    static float easeOutExponential(float p, float b, float c);
    static float easeInOutExponential(float p, float b, float c);
    static float easeInCircular(float p, float b, float c);
    static float easeOutCircular(float p, float b, float c);
    static float easeInOutCircular(float p, float b, float c);

    static std::unordered_map<std::string, TweenAlgorithm> tweenTypeMap_;
    static std::unordered_map<std::string, TweenProperty> tweenPropertyMap_;

    TweenAlgorithm type;
    float start;
    float end;
};