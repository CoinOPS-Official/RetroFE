#pragma once

#include "Restrictor.h"
#include <memory>
#include <future>

class RestrictorManager {
public:
    RestrictorManager();
    ~RestrictorManager();

    void startInitialization();
    bool isReady();

    static IRestrictor* getGlobalRestrictor();

private:
    std::future<std::unique_ptr<IRestrictor>> restrictorFuture_;
    std::unique_ptr<IRestrictor> restrictor_;
    static IRestrictor* gRestrictor;
};