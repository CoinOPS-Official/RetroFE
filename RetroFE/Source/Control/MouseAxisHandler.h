#pragma once
#include "InputHandler.h"
#include "UserInput.h"

class MouseAxisHandler : public InputHandler {
public:
    enum Axis { X = 0, Y = 1 };

    MouseAxisHandler(Axis axis, int direction, int threshold, UserInput& input);

    void reset() override;
    bool update(SDL_Event& e) override;
    bool pressed() override;
    void updateKeystate() override;

private:
    Axis axis_;
    int direction_;
    int threshold_;
    bool pressed_;
    UserInput& input_; // Reference to per-frame state
};