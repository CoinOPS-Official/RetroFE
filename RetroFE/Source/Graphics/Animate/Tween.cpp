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
#include "Tween.h"
#include "../../Utility/Log.h"
#include <algorithm>
#define _USE_MATH_DEFINES
#include <math.h>
#include <string>
#include <optional>

std::unordered_map<std::string, TweenAlgorithm> Tween::tweenTypeMap_ = {
    {"easeinquadratic", EASE_IN_QUADRATIC},
    {"easeoutquadratic", EASE_OUT_QUADRATIC},
    {"easeinoutquadratic", EASE_INOUT_QUADRATIC},
    {"easeincubic", EASE_IN_CUBIC},
    {"easeoutcubic", EASE_OUT_CUBIC},
    {"easeinoutcubic", EASE_INOUT_CUBIC},
    {"easeinquartic", EASE_IN_QUARTIC},
    {"easeoutquartic", EASE_OUT_QUARTIC},
    {"easeinoutquartic", EASE_INOUT_QUARTIC},
    {"easeinquintic", EASE_IN_QUINTIC},
    {"easeoutquintic", EASE_OUT_QUINTIC},
    {"easeinoutquintic", EASE_INOUT_QUINTIC}, // <-- Corrected "easeonoutquintic" typo
    {"easeinsine", EASE_IN_SINE},
    {"easeoutsine", EASE_OUT_SINE},
    {"easeinoutsine", EASE_INOUT_SINE},
    {"easeinexponential", EASE_IN_EXPONENTIAL},
    {"easeoutexponential", EASE_OUT_EXPONENTIAL},
    {"easeinoutexponential", EASE_INOUT_EXPONENTIAL},
    {"easeincircular", EASE_IN_CIRCULAR},
    {"easeoutcircular", EASE_OUT_CIRCULAR},
    {"easeinoutcircular", EASE_INOUT_CIRCULAR},
    {"linear", LINEAR}
};

std::unordered_map<std::string, TweenProperty> Tween::tweenPropertyMap_ = {
    {"x", TWEEN_PROPERTY_X},
    {"y", TWEEN_PROPERTY_Y},
    {"angle", TWEEN_PROPERTY_ANGLE},
    {"alpha", TWEEN_PROPERTY_ALPHA},
    {"width", TWEEN_PROPERTY_WIDTH},
    {"height", TWEEN_PROPERTY_HEIGHT},
    {"xorigin", TWEEN_PROPERTY_X_ORIGIN},
    {"yorigin", TWEEN_PROPERTY_Y_ORIGIN},
    {"xoffset", TWEEN_PROPERTY_X_OFFSET},
    {"yoffset", TWEEN_PROPERTY_Y_OFFSET},
    {"fontsize", TWEEN_PROPERTY_FONT_SIZE},
    {"backgroundalpha", TWEEN_PROPERTY_BACKGROUND_ALPHA},
    {"maxwidth", TWEEN_PROPERTY_MAX_WIDTH},
    {"maxheight", TWEEN_PROPERTY_MAX_HEIGHT},
    {"layer", TWEEN_PROPERTY_LAYER},
    {"containerx", TWEEN_PROPERTY_CONTAINER_X},
    {"containery", TWEEN_PROPERTY_CONTAINER_Y},
    {"containerwidth", TWEEN_PROPERTY_CONTAINER_WIDTH},
    {"containerheight", TWEEN_PROPERTY_CONTAINER_HEIGHT},
    {"volume", TWEEN_PROPERTY_VOLUME},
    {"nop", TWEEN_PROPERTY_NOP},
    {"restart", TWEEN_PROPERTY_RESTART}
};

Tween::Tween(TweenProperty property, TweenAlgorithm type, float start, float end, float duration, const std::string& playlistFilter)
    : property(property)
    , duration(duration)
    , playlistFilter(playlistFilter)
    , type(type)
    , start(start)
    , end(end) {
}

std::optional<TweenProperty> Tween::getTweenProperty(const std::string& name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    auto it = tweenPropertyMap_.find(key);
    if (it != tweenPropertyMap_.end()) {
        return it->second;
    }

    return std::nullopt;
}

void Tween::reinit(TweenProperty prop, TweenAlgorithm alg, float newStart, float newEnd, float newDuration, const std::string& playlist) {
    this->property = prop;
    this->type = alg;
    this->start = newStart;
    this->end = newEnd;
    this->duration = newDuration;
    this->playlistFilter = playlist;
    this->startDefined = true; // Recycled tweens from this method are always defined
}

TweenAlgorithm Tween::getTweenType(const std::string& name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    auto it = tweenTypeMap_.find(key);
    if (it != tweenTypeMap_.end())
        return it->second;

    return LINEAR;
}

float Tween::animate(double elapsedTime) const {
    return animateSingle(type, start, end, duration, static_cast<float>(elapsedTime));
}

float Tween::animate(double elapsedTime, float startValue) const {
    return animateSingle(type, startValue, end, duration, static_cast<float>(elapsedTime));
}

float Tween::animateSingle(TweenAlgorithm type, float start, float end, float duration, float elapsedTime) {
    // If duration is zero or negative, animation is instant. Return the end state.
    if (duration <= 0.0f) {
        return end;
    }

    // Clamp time to prevent overshooting the animation.
    elapsedTime = std::min(elapsedTime, duration);

    // OPTIMIZATION: Calculate normalized progress (0.0 to 1.0) once.
    const float progress = elapsedTime / duration;
    const float change = end - start;

    switch (type) {
        case EASE_IN_QUADRATIC:     return easeInQuadratic(progress, start, change);
        case EASE_OUT_QUADRATIC:    return easeOutQuadratic(progress, start, change);
        case EASE_INOUT_QUADRATIC:  return easeInOutQuadratic(progress, start, change);
        case EASE_IN_CUBIC:         return easeInCubic(progress, start, change);
        case EASE_OUT_CUBIC:        return easeOutCubic(progress, start, change);
        case EASE_INOUT_CUBIC:      return easeInOutCubic(progress, start, change);
        case EASE_IN_QUARTIC:       return easeInQuartic(progress, start, change);
        case EASE_OUT_QUARTIC:      return easeOutQuartic(progress, start, change);
        case EASE_INOUT_QUARTIC:    return easeInOutQuartic(progress, start, change);
        case EASE_IN_QUINTIC:       return easeInQuintic(progress, start, change);
        case EASE_OUT_QUINTIC:      return easeOutQuintic(progress, start, change);
        case EASE_INOUT_QUINTIC:    return easeInOutQuintic(progress, start, change);
        case EASE_IN_SINE:          return easeInSine(progress, start, change);
        case EASE_OUT_SINE:         return easeOutSine(progress, start, change);
        case EASE_INOUT_SINE:       return easeInOutSine(progress, start, change);
        case EASE_IN_EXPONENTIAL:   return easeInExponential(progress, start, change);
        case EASE_OUT_EXPONENTIAL:  return easeOutExponential(progress, start, change);
        case EASE_INOUT_EXPONENTIAL:return easeInOutExponential(progress, start, change);
        case EASE_IN_CIRCULAR:      return easeInCircular(progress, start, change);
        case EASE_OUT_CIRCULAR:     return easeOutCircular(progress, start, change);
        case EASE_INOUT_CIRCULAR:   return easeInOutCircular(progress, start, change);
        case LINEAR:
        default:                    return linear(progress, start, change);
    }
}

// NOTE: All easing functions now use the new signature:
// (float progress, float start_value, float change_in_value)
// 'p' is progress (0.0 to 1.0), 'b' is the beginning value, 'c' is the total change.

float Tween::linear(float p, float b, float c) {
    return c * p + b;
}

float Tween::easeInQuadratic(float p, float b, float c) {
    return c * p * p + b;
}

float Tween::easeOutQuadratic(float p, float b, float c) {
    return -c * p * (p - 2.0f) + b;
}

float Tween::easeInOutQuadratic(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return c / 2.0f * p * p + b;
    p--;
    return -c / 2.0f * (p * (p - 2.0f) - 1.0f) + b;
}

float Tween::easeInCubic(float p, float b, float c) {
    return c * p * p * p + b;
}

float Tween::easeOutCubic(float p, float b, float c) {
    p--;
    return c * (p * p * p + 1.0f) + b;
}

float Tween::easeInOutCubic(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return c / 2.0f * p * p * p + b;
    p -= 2.0f;
    return c / 2.0f * (p * p * p + 2.0f) + b;
}

float Tween::easeInQuartic(float p, float b, float c) {
    return c * p * p * p * p + b;
}

float Tween::easeOutQuartic(float p, float b, float c) {
    p--;
    return -c * (p * p * p * p - 1.0f) + b;
}

float Tween::easeInOutQuartic(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return c / 2.0f * p * p * p * p + b;
    p -= 2.0f;
    return -c / 2.0f * (p * p * p * p - 2.0f) + b;
}

float Tween::easeInQuintic(float p, float b, float c) {
    return c * p * p * p * p * p + b;
}

float Tween::easeOutQuintic(float p, float b, float c) {
    p--;
    return c * (p * p * p * p * p + 1.0f) + b;
}

float Tween::easeInOutQuintic(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return c / 2.0f * p * p * p * p * p + b;
    p -= 2.0f;
    return c / 2.0f * (p * p * p * p * p + 2.0f) + b;
}

float Tween::easeInSine(float p, float b, float c) {
    return -c * cosf(p * ((float)M_PI / 2.0f)) + c + b;
}

float Tween::easeOutSine(float p, float b, float c) {
    return c * sinf(p * ((float)M_PI / 2.0f)) + b;
}

float Tween::easeInOutSine(float p, float b, float c) {
    return -c / 2.0f * (cosf((float)M_PI * p) - 1.0f) + b;
}

float Tween::easeInExponential(float p, float b, float c) {
    return c * powf(2.0f, 10.0f * (p - 1.0f)) + b;
}

float Tween::easeOutExponential(float p, float b, float c) {
    return c * (-powf(2.0f, -10.0f * p) + 1.0f) + b;
}

float Tween::easeInOutExponential(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return c / 2.0f * powf(2.0f, 10.0f * (p - 1.0f)) + b;
    p--;
    return c / 2.0f * (-powf(2.0f, -10.0f * p) + 2.0f) + b;
}

float Tween::easeInCircular(float p, float b, float c) {
    return -c * (sqrtf(1.0f - p * p) - 1.0f) + b;
}

float Tween::easeOutCircular(float p, float b, float c) {
    p--;
    return c * sqrtf(1.0f - p * p) + b;
}

float Tween::easeInOutCircular(float p, float b, float c) {
    p *= 2.0f;
    if (p < 1.0f) return -c / 2.0f * (sqrtf(1.0f - p * p) - 1.0f) + b;
    p -= 2.0f;
    return c / 2.0f * (sqrtf(1.0f - p * p) + 1.0f);
}