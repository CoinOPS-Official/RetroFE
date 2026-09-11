#pragma once

#include "InputHandler.h"

class JoyButtonHandler : public InputHandler
{
public:
    JoyButtonHandler(int joynum, Uint8 button);
    bool update(SDL_Event &e);
    bool pressed();
    void reset();
	void updateKeystate() {};

private:
    int joynum_;
    Uint8 button_;
    bool pressed_{ false };
};

