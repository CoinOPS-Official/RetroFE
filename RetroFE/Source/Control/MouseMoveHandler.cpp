#include "MouseMoveHandler.h"

MouseMoveHandler::MouseMoveHandler(Axis axis, int direction, int threshold, UserInput::KeyCode_E boundKeyCode)
    : axis_(axis), direction_(direction), threshold_(threshold), pressed_(false), boundKeyCode_(boundKeyCode) {
}

void MouseMoveHandler::reset() {
    pressed_ = false;
}

bool MouseMoveHandler::update(SDL_Event& e) {
    if (e.type == SDL_MOUSEMOTION) {
        int delta = (axis_ == X) ? e.motion.xrel : e.motion.yrel;

        // If the movement in the correct direction is >= our threshold...
        if ((delta * direction_) >= threshold_) {
            // ...the key is pressed for this frame.
            pressed_ = true;
        }
    }
    return pressed_;
}

bool MouseMoveHandler::pressed() {
    return pressed_;
}

void MouseMoveHandler::updateKeystate() {
    // This model does not need this function.
}