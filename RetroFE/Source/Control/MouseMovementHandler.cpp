#include "MouseMovementHandler.h"
#include <SDL2/SDL.h>
#include "../SDL.h"

MouseMovementHandler::MouseMovementHandler(Uint8 button)
: button_(button)
, pressed_(false)
, lastpos_(0)
{
}

void MouseMovementHandler::reset()
{
    pressed_= false;
}

bool MouseMovementHandler::update(SDL_Event &e)
{
    // if postion different then pressed
    if (e.type == SDL_MOUSEMOTION && !pressed_) {
        // to do these might need to be settings to tweek
        int buffer = 50;
        int extent = 20 + buffer;
        int w = SDL::getWindowWidth(0);
        int h = SDL::getWindowHeight(0);

        // todo replace numbers with enum
        if (button_ == 61 && e.motion.xrel < -buffer && e.motion.xrel > -extent) {
            pressed_ = true;
        } else if (button_ == 62 && e.motion.xrel > buffer && e.motion.xrel < extent) {
            pressed_ = true;
        } else if (button_ == 71 && e.motion.yrel < -buffer && e.motion.yrel > -extent) {
            pressed_ = true;
        } else if (button_ == 72 && e.motion.yrel > buffer && e.motion.yrel < extent) {
            pressed_ = true;
        }

        if (pressed_ || e.motion.x == 0 || e.motion.y == 0 || e.motion.x == w || e.motion.y == h ) {
            SDL_WarpMouseInWindow(SDL::getWindow(0),w / 2, h / 2);
        }
    }
    else {
        pressed_ = false;
    }


    return pressed_;
}

bool MouseMovementHandler::pressed()
{
    return pressed_;
}

