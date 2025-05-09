#pragma once
#include <optional>
#include <memory>

class IRestrictor {
public:
    virtual ~IRestrictor() = default;

    virtual bool initialize() = 0;
    virtual bool setWay(int way) = 0;
    virtual std::optional<int> getWay() = 0;

    static std::unique_ptr<IRestrictor> create();
};
