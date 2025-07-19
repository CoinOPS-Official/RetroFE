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
    {"easeonoutquintic", EASE_INOUT_QUINTIC}, // Note: Typo in original, likely meant "easeinoutquintic"
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

// Assuming Tween members start, end, and duration were changed from double to float in Tween.h
Tween::Tween(TweenProperty property, TweenAlgorithm type, float start, float end, float duration, const std::string& playlistFilter)
    : property(property)
    , duration(duration)
    , playlistFilter(playlistFilter)
    , type(type)
    , start(start)
    , end(end) {
}


bool Tween::getTweenProperty(const std::string& name, TweenProperty& property) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    auto it = tweenPropertyMap_.find(key);
    if (it != tweenPropertyMap_.end()) {
        property = it->second;
        return true;
    }

    return false;
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

// All animation calculations are now done using floats for performance and consistency.
float Tween::animateSingle(TweenAlgorithm type, float start, float end, float duration, float elapsedTime) {
    float a = start;
    float b = end - start;
    float result = 0.0f;

    switch (type) {
        case EASE_IN_QUADRATIC:
        result = easeInQuadratic(elapsedTime, duration, a, b);
        break;

        case EASE_OUT_QUADRATIC:
        result = easeOutQuadratic(elapsedTime, duration, a, b);
        break;

        case EASE_INOUT_QUADRATIC:
        result = easeInOutQuadratic(elapsedTime, duration, a, b);
        break;

        case EASE_IN_CUBIC:
        result = easeInCubic(elapsedTime, duration, a, b);
        break;

        case EASE_OUT_CUBIC:
        result = easeOutCubic(elapsedTime, duration, a, b);
        break;

        case EASE_INOUT_CUBIC:
        result = easeInOutCubic(elapsedTime, duration, a, b);
        break;

        case EASE_IN_QUARTIC:
        result = easeInQuartic(elapsedTime, duration, a, b);
        break;

        case EASE_OUT_QUARTIC:
        result = easeOutQuartic(elapsedTime, duration, a, b);
        break;

        case EASE_INOUT_QUARTIC:
        result = easeInOutQuartic(elapsedTime, duration, a, b);
        break;

        case EASE_IN_QUINTIC:
        result = easeInQuintic(elapsedTime, duration, a, b);
        break;

        case EASE_OUT_QUINTIC:
        result = easeOutQuintic(elapsedTime, duration, a, b);
        break;

        case EASE_INOUT_QUINTIC:
        result = easeInOutQuintic(elapsedTime, duration, a, b);
        break;

        case EASE_IN_SINE:
        result = easeInSine(elapsedTime, duration, a, b);
        break;

        case EASE_OUT_SINE:
        result = easeOutSine(elapsedTime, duration, a, b);
        break;

        case EASE_INOUT_SINE:
        result = easeInOutSine(elapsedTime, duration, a, b);
        break;

        case EASE_IN_EXPONENTIAL:
        result = easeInExponential(elapsedTime, duration, a, b);
        break;

        case EASE_OUT_EXPONENTIAL:
        result = easeOutExponential(elapsedTime, duration, a, b);
        break;

        case EASE_INOUT_EXPONENTIAL:
        result = easeInOutExponential(elapsedTime, duration, a, b);
        break;

        case EASE_IN_CIRCULAR:
        result = easeInCircular(elapsedTime, duration, a, b);
        break;

        case EASE_OUT_CIRCULAR:
        result = easeOutCircular(elapsedTime, duration, a, b);
        break;

        case EASE_INOUT_CIRCULAR:
        result = easeInOutCircular(elapsedTime, duration, a, b);
        break;

        case LINEAR:
        default:
        result = linear(elapsedTime, duration, a, b);
        break;
    }

    return result;

}

float Tween::linear(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    return c * t / d + b;
};

float Tween::easeInQuadratic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d;
    return c * t * t + b;
};

float Tween::easeOutQuadratic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d;
    return -c * t * (t - 2.0f) + b;
};

float Tween::easeInOutQuadratic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d / 2.0f;
    if (t < 1.0f) return c / 2.0f * t * t + b;
    t--;
    return -c / 2.0f * (t * (t - 2.0f) - 1.0f) + b;
};

float Tween::easeInCubic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d;
    return c * t * t * t + b;
};

float Tween::easeOutCubic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d;
    t--;
    return c * (t * t * t + 1.0f) + b;
};

float Tween::easeInOutCubic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d / 2.0f;
    if (t < 1.0f) return c / 2.0f * t * t * t + b;
    t -= 2.0f;
    return c / 2.0f * (t * t * t + 2.0f) + b;
};

float Tween::easeInQuartic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d;
    return c * t * t * t * t + b;
};

float Tween::easeOutQuartic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d;
    t--;
    return -c * (t * t * t * t - 1.0f) + b;
};

float Tween::easeInOutQuartic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d / 2.0f;
    if (t < 1.0f) return c / 2.0f * t * t * t * t + b;
    t -= 2.0f;
    return -c / 2.0f * (t * t * t * t - 2.0f) + b;
};

float Tween::easeInQuintic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d;
    return c * t * t * t * t * t + b;
};


float Tween::easeOutQuintic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d;
    t--;
    return c * (t * t * t * t * t + 1.0f) + b;
};

float Tween::easeInOutQuintic(float t, float d, float b, float c) {
    if (d == 0.0f) return b;
    t /= d / 2.0f;
    if (t < 1.0f) return c / 2.0f * t * t * t * t * t + b;
    t -= 2.0f;
    return c / 2.0f * (t * t * t * t * t + 2.0f) + b;
};

float Tween::easeInSine(float t, float d, float b, float c) {
    return -c * cosf(t / d * ((float)M_PI / 2.0f)) + c + b;
};

float Tween::easeOutSine(float t, float d, float b, float c) {
    return c * sinf(t / d * ((float)M_PI / 2.0f)) + b;
};

float Tween::easeInOutSine(float t, float d, float b, float c) {
    return -c / 2.0f * (cosf((float)M_PI * t / d) - 1.0f) + b;
};

float Tween::easeInExponential(float t, float d, float b, float c) {
    return c * powf(2.0f, 10.0f * (t / d - 1.0f)) + b;
};

float Tween::easeOutExponential(float t, float d, float b, float c) {
    return c * (-powf(2.0f, -10.0f * t / d) + 1.0f) + b;
};

float Tween::easeInOutExponential(float t, float d, float b, float c) {
    t /= d / 2.0f;
    if (t < 1.0f) return c / 2.0f * powf(2.0f, 10.0f * (t - 1.0f)) + b;
    t--;
    return c / 2.0f * (-1.0f * powf(2.0f, -10.0f * t) + 2.0f) + b;
};

float Tween::easeInCircular(float t, float d, float b, float c) {
    t /= d;
    return -c * (sqrtf(1.0f - t * t) - 1.0f) + b;
};


float Tween::easeOutCircular(float t, float d, float b, float c) {
    t /= d;
    t--;
    return c * sqrtf(1.0f - t * t) + b;
};

float Tween::easeInOutCircular(float t, float d, float b, float c) {
    t /= d / 2.0f;
    if (t < 1.0f) return -c / 2.0f * (sqrtf(1.0f - t * t) - 1.0f) + b;
    t -= 2.0f;
    return c / 2.0f * (sqrtf(1.0f - t * t) + 1.0f) + b;
}