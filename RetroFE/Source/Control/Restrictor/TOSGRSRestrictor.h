#pragma once
#include "Restrictor.h"
#include "libserialport.h"
#include <optional>
#include <string>

class TOSGRSRestrictor : public IRestrictor {
public:
    TOSGRSRestrictor(uint16_t vid = 0x2341, uint16_t pid = 0x8036);
    ~TOSGRSRestrictor() override;

    bool initialize() override;
    bool setWay(int way) override;
    std::optional<int> getWay() override;

    static bool isPresent();

private:
    uint16_t vid_, pid_;
    sp_port* port_;

    sp_port* findPort(uint16_t vid, uint16_t pid);
    std::string sendCmd(const std::string&);
};
