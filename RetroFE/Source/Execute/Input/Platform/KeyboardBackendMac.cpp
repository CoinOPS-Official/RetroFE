#ifdef __APPLE__
#include "../KeyboardBackendFactory.h"
class KbMac final : public IKeyboardBackend {
public:
    int  mapKeyName(const std::string&) override { return -1; }
    void setSingleQuitKeys(const std::vector<int>&) override {}
    void setComboQuitKeys(const std::vector<int>&) override {}
    bool poll(const std::function<void(int, bool)>&) override { return false; }
};
std::unique_ptr<IKeyboardBackend> makeKeyboardBackend() {
    return std::unique_ptr<IKeyboardBackend>(new KbMac());
}
#endif