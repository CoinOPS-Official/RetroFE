#pragma once
#include "InputHandler.h"
#include "UserInput.h"
#include <SDL2/SDL_events.h>

class TouchDragHandler : public InputHandler {
public:
    enum DragAxis { X_AXIS, Y_AXIS };
    TouchDragHandler(DragAxis axis, int direction, int threshold, UserInput::KeyCode_E boundKeyCode);
    void reset() override;
    bool update(SDL_Event& e) override;
    bool pressed() override;
    void updateKeystate() override;
private:
    DragAxis axis_;
    int direction_;
    int threshold_;
    UserInput::KeyCode_E boundKeyCode_;
    bool pressed_;
    bool isTracking_;
    Sint64 trackingFingerId_;
    float anchorX_, anchorY_;
    float currentX_, currentY_;
    float accumulator_;
};