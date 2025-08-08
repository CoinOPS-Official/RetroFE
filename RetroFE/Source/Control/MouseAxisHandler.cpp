#include "MouseAxisHandler.h"
#include "UserInput.h"
#include <cmath>

MouseAxisHandler::MouseAxisHandler(Axis axis, int direction, int threshold, UserInput& input)
    : axis_(axis), direction_(direction), threshold_(threshold), pressed_(false), input_(input) {
}

void MouseAxisHandler::reset() {
    pressed_ = false;
}

bool MouseAxisHandler::update(SDL_Event& e) {
    pressed_ = false;
    if (e.type == SDL_MOUSEMOTION) {
        int absX = std::abs(input_.getTotalXrel());
        int absY = std::abs(input_.getTotalYrel());
        bool xPast = absX >= threshold_;
        bool yPast = absY >= threshold_;

        // Diagonal deadzone: ignore if both past threshold
        if (xPast && yPast)
            return false;

        // Only activate for our axis if the other is not active
        if ((axis_ == X && xPast && !yPast) ||
            (axis_ == Y && yPast && !xPast)) {
            int delta = (axis_ == X) ? input_.getTotalXrel() : input_.getTotalYrel();
            if ((delta * direction_) >= threshold_) {
                pressed_ = true;
            }
        }
    }
    return pressed_;
}

bool MouseAxisHandler::pressed() {
    return pressed_;
}

void MouseAxisHandler::updateKeystate() {
    // This model does not need this function.
}