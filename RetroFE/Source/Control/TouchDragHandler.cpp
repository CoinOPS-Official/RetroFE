#include "TouchDragHandler.h"
#include "../Utility/Log.h"
#include <cmath>

// --- Tuning Constants ---
// Determines how much the scroll speed changes based on drag distance.
// This is the main "sensitivity" control for the acceleration.
const float ACCELERATION_FACTOR = 0.5f;

// The amount of "friction" applied each frame. This helps the scrolling
// feel smooth and ensures it stops when you return to the anchor.
const float FRICTION = 0.85f;


TouchDragHandler::TouchDragHandler(DragAxis axis, int direction, int threshold)
    : axis_(axis), direction_(direction), threshold_(threshold),
    pressed_(false), isTracking_(false), trackingFingerId_(-1),
    anchorX_(0.0f), anchorY_(0.0f), currentX_(0.0f), currentY_(0.0f), accumulator_(0.0f) {
}

void TouchDragHandler::reset() {
    // We only reset the final output state here.
    pressed_ = false;
}

bool TouchDragHandler::update(SDL_Event& e) {
    if (e.type == SDL_FINGERDOWN) {
        if (!isTracking_) {
            isTracking_ = true;
            trackingFingerId_ = e.tfinger.fingerId;
            anchorX_ = currentX_ = e.tfinger.x;
            anchorY_ = currentY_ = e.tfinger.y;
            accumulator_ = 0.0f;
        }
    }
    else if (e.type == SDL_FINGERMOTION) {
        if (isTracking_ && e.tfinger.fingerId == trackingFingerId_) {
            // Motion events just update the finger's current position.
            currentX_ = e.tfinger.x;
            currentY_ = e.tfinger.y;
        }
    }
    else if (e.type == SDL_FINGERUP) {
        if (isTracking_ && e.tfinger.fingerId == trackingFingerId_) {
            isTracking_ = false;
            accumulator_ = 0.0f;
        }
    }
    return false;
}

bool TouchDragHandler::pressed() {
    return pressed_;
}

// This function is the core of the model, called once per frame.
void TouchDragHandler::updateKeystate() {
    // If we are not tracking a finger, apply friction until the accumulator is zero.
    if (!isTracking_) {
        accumulator_ *= FRICTION;
        if (accumulator_ < 1.0f) accumulator_ = 0.0f;
        pressed_ = (accumulator_ >= threshold_);
        return;
    }

    float current_pos = (axis_ == X_AXIS) ? currentX_ : currentY_;
    float anchor_pos = (axis_ == X_AXIS) ? anchorX_ : anchorY_;

    // Calculate the distance from the anchor, scaled up.
    float distance_from_anchor = (current_pos - anchor_pos) * 1000.0f;

    // Determine the distance in the direction we care about.
    float directed_distance = distance_from_anchor * direction_;

    // --- The Corrected Logic ---
    // The "target velocity" is how far the finger is past the dead zone.
    // If the finger is inside the dead zone, the target is 0.
    float target_velocity = 0.0f;
    if (directed_distance > threshold_) {
        target_velocity = directed_distance - threshold_;
    }

    // The accumulator now smoothly moves towards the target velocity.
    // This is a simple linear interpolation (lerp).
    accumulator_ = (accumulator_ * FRICTION) + (target_velocity * (1.0f - FRICTION) * ACCELERATION_FACTOR);

    // --- State Check ---
    // Check if the current energy in the accumulator is enough to trigger a press.
    if (accumulator_ >= threshold_) {
        pressed_ = true;
    }
    else {
        pressed_ = false;
    }

    // Clean up small values.
    if (accumulator_ < 1.0f) {
        accumulator_ = 0.0f;
    }
}