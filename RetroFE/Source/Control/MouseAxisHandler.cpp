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

    return pressed_;
}

bool MouseAxisHandler::pressed() {
    return pressed_;
}

void MouseAxisHandler::updateKeystate() {
    // This model does not need this function.
}