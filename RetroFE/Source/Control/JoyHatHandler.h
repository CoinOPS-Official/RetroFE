#pragma once

#include "InputHandler.h"

class JoyHatHandler : public InputHandler
{
public:
    JoyHatHandler(int joynum, Uint8 hatnum, Uint8 direction);
    bool update(SDL_Event &e);
    bool pressed();
    void reset();
	void updateKeystate() {};

private:
    int joynum_;
    Uint8 hatnum_;
    Uint8 direction_;
    bool pressed_{ false };
};

