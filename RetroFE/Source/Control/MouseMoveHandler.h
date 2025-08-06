#pragma once
#include "InputHandler.h"
#include "UserInput.h"

class MouseMoveHandler : public InputHandler {
public:
    enum Axis { X = 0, Y = 1 };
    MouseMoveHandler(Axis axis, int direction, int threshold, UserInput::KeyCode_E boundKeyCode);
    void reset() override;
    bool update(SDL_Event& e) override;
    bool pressed() override;
    void updateKeystate() override;
private:
    Axis axis_;
    int direction_;
    int threshold_;
    bool pressed_;
    UserInput::KeyCode_E boundKeyCode_;
};