#include "KeyboardHandler.h"

KeyboardHandler::KeyboardHandler(SDL_Scancode s)
: scancode_(s)
{
}

void KeyboardHandler::reset()
{
    pressed_= false;
}

bool KeyboardHandler::update(SDL_Event &e)
{
    if(e.type != SDL_EVENT_KEY_UP && e.type != SDL_EVENT_KEY_DOWN) return false;

    if(e.key.scancode == scancode_) {
        pressed_ = (e.type == SDL_EVENT_KEY_DOWN);
        return true;
    }

    return false;
}

bool KeyboardHandler::pressed()
{
    return pressed_;
}

void KeyboardHandler::updateKeystate()
{
	const bool *state = SDL_GetKeyboardState(nullptr);
	pressed_ = state[scancode_];
}
