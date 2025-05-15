#include "Restrictor.h"
#include "TOSGRSRestrictor.h"
#include "ServoStikRestrictor.h"

std::unique_ptr<IRestrictor> IRestrictor::create() {
	auto grs = std::make_unique<TOSGRSRestrictor>();
	if (grs->initialize()) {
		grs->setWay(8);

		return grs;
	}
	auto servo = std::make_unique<ServoStikRestrictor>();
	if (servo->initialize()) {
		servo->setWay(8);
		return servo;
	}

	return nullptr;
}