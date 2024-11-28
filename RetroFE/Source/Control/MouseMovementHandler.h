#pragma once

#include "InputHandler.h"
#include "SDL.h"

class MouseMovementHandler : public InputHandler
{
public:
    MouseMovementHandler(Uint8 button);
    bool update(SDL_Event &e);
    bool pressed();
    void reset();
	void updateKeystate() {};

private:
    Uint8 button_;
    bool pressed_;
    Sint32 lastpos_;
};

