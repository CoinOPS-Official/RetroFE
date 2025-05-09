#pragma once
#include "Restrictor.h"
#ifdef _WIN32
#include <Windows.h>
#else
#include <libusb.h>
#endif

class ServoStikRestrictor : public IRestrictor {
public:
    ServoStikRestrictor(uint16_t vid = 0xD209, uint16_t pid = 0x1700);
    ~ServoStikRestrictor() override;

    bool initialize() override;
    bool setWay(int way) override;
    std::optional<int> getWay() override;

    static bool isPresent();

private:
    uint16_t vid_, pid_;
#ifdef _WIN32
    bool initialized_;
#else
    libusb_context* ctx_;
    libusb_device_handle* handle_;
#endif
};
