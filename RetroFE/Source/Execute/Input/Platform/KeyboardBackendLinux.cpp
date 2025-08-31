#ifdef __linux__
#include "../KeyboardBackendFactory.h"
#include "../IKeyboardBackend.h"
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>

namespace {
    int toEvdev(std::string s) {
        for (auto& c : s) c = (char)toupper((unsigned char)c);
        if (s == "ESC") s = "ESCAPE";
        if (s == "RETURN") s = "ENTER";
        if (s.rfind("KEYPAD ", 0) == 0) { // "Keypad 8"
            if (s == "KEYPAD ENTER") s = "KPENTER";
            else if (s.size() >= 8 && isdigit((unsigned char)s[7])) s = "KP" + s.substr(7, 1);
        }
        static const std::unordered_map<std::string, int> m = {
            {"A",KEY_A},{"B",KEY_B},{"C",KEY_C},{"D",KEY_D},{"E",KEY_E},{"F",KEY_F},
            {"G",KEY_G},{"H",KEY_H},{"I",KEY_I},{"J",KEY_J},{"K",KEY_K},{"L",KEY_L},
            {"M",KEY_M},{"N",KEY_N},{"O",KEY_O},{"P",KEY_P},{"Q",KEY_Q},{"R",KEY_R},
            {"S",KEY_S},{"T",KEY_T},{"U",KEY_U},{"V",KEY_V},{"W",KEY_W},{"X",KEY_X},
            {"Y",KEY_Y},{"Z",KEY_Z},
            {"0",KEY_0},{"1",KEY_1},{"2",KEY_2},{"3",KEY_3},{"4",KEY_4},
            {"5",KEY_5},{"6",KEY_6},{"7",KEY_7},{"8",KEY_8},{"9",KEY_9},
            {"F1",KEY_F1},{"F2",KEY_F2},{"F3",KEY_F3},{"F4",KEY_F4},{"F5",KEY_F5},
            {"F6",KEY_F6},{"F7",KEY_F7},{"F8",KEY_F8},{"F9",KEY_F9},{"F10",KEY_F10},
            {"F11",KEY_F11},{"F12",KEY_F12},
            {"LEFT",KEY_LEFT},{"RIGHT",KEY_RIGHT},{"UP",KEY_UP},{"DOWN",KEY_DOWN},
            {"ESCAPE",KEY_ESC},{"SPACE",KEY_SPACE},{"TAB",KEY_TAB},{"ENTER",KEY_ENTER},
            {"BACKSPACE",KEY_BACKSPACE},
            {"KP0",KEY_KP0},{"KP1",KEY_KP1},{"KP2",KEY_KP2},{"KP3",KEY_KP3},{"KP4",KEY_KP4},
            {"KP5",KEY_KP5},{"KP6",KEY_KP6},{"KP7",KEY_KP7},{"KP8",KEY_KP8},{"KP9",KEY_KP9},
            {"KPENTER",KEY_KPENTER}
        };
        auto it = m.find(s);
        return it == m.end() ? -1 : it->second;
    }

    struct Dev { int fd = -1; libevdev* dev = nullptr; };
    int open_ro_nb(const char* path) { return open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); }
}

class KbLinux final : public IKeyboardBackend {
public:
    KbLinux() { initUdev_(); enumerate_(); }
    ~KbLinux() override { closeAll_(); }

    int mapKeyName(const std::string& n) override { return toEvdev(n); }
    void setSingleQuitKeys(const std::vector<int>& v) override { singles_ = v; }
    void setComboQuitKeys(const std::vector<int>& v) override { combo_ = v; }

    bool poll(const std::function<void(int, bool)>& onKey) override {
        bool any = false;

        for (auto& d : devs_) {
            if (!d.dev) continue;

            for (;;) {
                input_event ev{};
                int rc = libevdev_next_event(d.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);

                if (rc == -EAGAIN) {
                    // no more events right now for this device
                    break;
                }

                if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                    // Device fell behind: drain the sync queue so we don't miss edges
                    do {
                        if (ev.type == EV_KEY && (ev.value == 1 || ev.value == 0)) {
                            onKey(ev.code, ev.value == 1);
                            any = true;
                        }
                        rc = libevdev_next_event(d.dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
                    } while (rc == LIBEVDEV_READ_STATUS_SYNC);
                    // go back to normal reads
                    continue;
                }

                if (rc < 0) {
                    // Device error; if it was unplugged, close it
                    if (rc == -ENODEV) {
                        if (d.dev) { libevdev_free(d.dev); d.dev = nullptr; }
                        if (d.fd >= 0) { close(d.fd); d.fd = -1; }
                    }
                    break;
                }

                // rc == LIBEVDEV_READ_STATUS_SUCCESS
                if (ev.type == EV_KEY && (ev.value == 1 || ev.value == 0)) {
                    onKey(ev.code, ev.value == 1);
                    any = true;
                }
            }
        }

        return any;
    }


private:
    std::vector<int> singles_, combo_;
    std::vector<Dev> devs_;
    udev* udev_ = nullptr;

    void initUdev_() { udev_ = udev_new(); }
    void enumerate_() {
        if (!udev_) return;
        udev_enumerate* en = udev_enumerate_new(udev_);
        udev_enumerate_add_match_subsystem(en, "input");
        udev_enumerate_scan_devices(en);
        for (auto* it = udev_enumerate_get_list_entry(en); it; it = udev_list_entry_get_next(it)) {
            udev_device* d = udev_device_new_from_syspath(udev_, udev_list_entry_get_name(it));
            if (!d) continue;
            const char* node = udev_device_get_devnode(d);
            const char* isKb = udev_device_get_property_value(d, "ID_INPUT_KEYBOARD");
            if (node && isKb && !strcmp(isKb, "1")) {
                int fd = open_ro_nb(node);
                if (fd >= 0) {
                    libevdev* ld = nullptr;
                    if (libevdev_new_from_fd(fd, &ld) == 0 && libevdev_has_event_type(ld, EV_KEY)) {
                        devs_.push_back({ fd,ld });
                    }
                    else { if (ld) libevdev_free(ld); close(fd); }
                }
            }
            udev_device_unref(d);
        }
        udev_enumerate_unref(en);
    }
    void closeAll_() {
        for (auto& d : devs_) { if (d.dev) libevdev_free(d.dev); if (d.fd >= 0) close(d.fd); }
        devs_.clear();
        if (udev_) { udev_unref(udev_); udev_ = nullptr; }
    }
};

std::unique_ptr<IKeyboardBackend> makeKeyboardBackend() {
    return std::unique_ptr<IKeyboardBackend>(new KbLinux());
}
#endif