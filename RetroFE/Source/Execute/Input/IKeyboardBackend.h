#pragma once
#include <string>
#include <vector>
#include <functional>

class IKeyboardBackend {
public:
    virtual ~IKeyboardBackend() = default;
    virtual int  mapKeyName(const std::string& name) = 0;                  // "Q", "Escape", "Keypad 8", ...
    virtual void setSingleQuitKeys(const std::vector<int>& keys) = 0;      // OR-semantics
    virtual void setComboQuitKeys(const std::vector<int>& keys) = 0;       // AND within 200ms
    virtual bool poll(const std::function<void(int code, bool down)>& onKey) = 0; // non-blocking
};
