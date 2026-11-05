// The car this app builds. Each vertical is self-contained in verticals/<name>/ (its code, its part
// of the agent, its tests, its dashboard panel); to add one, write it there and add a line here and
// in the agent's include_files. The order is the dashboard's.

#include "Car.h"

#include "Climate.h"
#include "Drive.h"
#include "Media.h"
#include "Messages.h"
#include "Navigation.h"
#include "Phone.h"
#include "Profiles.h"

namespace car {

std::shared_ptr<Car> makeCar() {
    auto car = std::make_shared<Car>();
    car->add<Drive>();
    car->add<Climate>();
    car->add<Media>();
    car->add<Phone>();
    car->add<Messages>();
    car->add<Navigation>();
    car->add<Profiles>();
    return car;
}

} // namespace car
