#pragma once
#include <memory>
#include "IKeyboardBackend.h"

std::unique_ptr<IKeyboardBackend> makeKeyboardBackend(); // implemented per-OS