#ifdef WIN32
#include "../KeyboardBackendFactory.h"
#include "../IKeyboardBackend.h"
#include <windows.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <cctype>

namespace {
    int toVK(std::string s) {
        for (auto& c : s) c = (char)toupper((unsigned char)c);
        if (s == "ESC") s = "ESCAPE";
        if (s == "RETURN") s = "ENTER";
        if (s.rfind("KEYPAD ", 0) == 0) {
            if (s == "KEYPAD ENTER") s = "NPENTER";
            else if (s.size() >= 8 && isdigit((unsigned char)s[7])) s = "NP" + s.substr(7, 1);
        }
        static const std::unordered_map<std::string, int> m = {
            {"A",'A'},{"B",'B'},{"C",'C'},{"D",'D'},{"E",'E'},{"F",'F'},
            {"G",'G'},{"H",'H'},{"I",'I'},{"J",'J'},{"K",'K'},{"L",'L'},
            {"M",'M'},{"N",'N'},{"O",'O'},{"P",'P'},{"Q",'Q'},{"R",'R'},
            {"S",'S'},{"T",'T'},{"U",'U'},{"V",'V'},{"W",'W'},{"X",'X'},
            {"Y",'Y'},{"Z",'Z'},
            {"0",'0'},{"1",'1'},{"2",'2'},{"3",'3'},{"4",'4'},
            {"5",'5'},{"6",'6'},{"7",'7'},{"8",'8'},{"9",'9'},
            {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},{"F5",VK_F5},
            {"F6",VK_F6},{"F7",VK_F7},{"F8",VK_F8},{"F9",VK_F9},{"F10",VK_F10},
            {"F11",VK_F11},{"F12",VK_F12},
            {"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},{"UP",VK_UP},{"DOWN",VK_DOWN},
            {"ESCAPE",VK_ESCAPE},{"SPACE",VK_SPACE},{"TAB",VK_TAB},{"ENTER",VK_RETURN},
            {"BACKSPACE",VK_BACK},
            {"NP0",VK_NUMPAD0},{"NP1",VK_NUMPAD1},{"NP2",VK_NUMPAD2},{"NP3",VK_NUMPAD3},{"NP4",VK_NUMPAD4},
            {"NP5",VK_NUMPAD5},{"NP6",VK_NUMPAD6},{"NP7",VK_NUMPAD7},{"NP8",VK_NUMPAD8},{"NP9",VK_NUMPAD9},
            {"NPENTER",VK_RETURN}
        };
        auto it = m.find(s); return it == m.end() ? -1 : it->second;
    }
}

class KbWin final : public IKeyboardBackend {
public:
    int  mapKeyName(const std::string& n) override { return toVK(n); }
    void setSingleQuitKeys(const std::vector<int>& v) override { singles_ = v; prev_.assign(256, false); }
    void setComboQuitKeys(const std::vector<int>& v) override { combo_ = v;   prev_.assign(256, false); }
    bool poll(const std::function<void(int, bool)>& onKey) override {
        bool any = false;
        auto scan = [&](int vk) {
            SHORT s = GetAsyncKeyState(vk);
            bool down = (s & 0x8000) != 0;
            const int idx = vk & 0xFF;

            bool was = static_cast<bool>(prev_[idx]);  // read proxy -> bool
            if (down != was) {
                prev_[idx] = down;                     // write through proxy
                onKey(vk, down);
                any = true;
            }
            };

        for (int v : singles_) scan(v);
        for (int v : combo_)   scan(v);
        return any;
    }
private:
    std::vector<int> singles_, combo_;
    std::vector<bool> prev_;
};

std::unique_ptr<IKeyboardBackend> makeKeyboardBackend() {
    return std::unique_ptr<IKeyboardBackend>(new KbWin());
}
#endif