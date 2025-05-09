#include "Restrictor.h"
#include "TOSGRSRestrictor.h"
#include "ServoStikRestrictor.h"

std::unique_ptr<IRestrictor> IRestrictor::create() {
    auto grs = std::make_unique<TOSGRSRestrictor>();
    if (grs->initialize()) return grs;

    auto servo = std::make_unique<ServoStikRestrictor>();
    if (servo->initialize()) return servo;

    return nullptr;
}