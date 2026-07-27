#include "VASDK/context/LocationContextProvider.h"

namespace va {
    namespace context {
        void LocationContextProvider::setLocationInfo(const LocationInfo& info) {
            _locationInfo = info;
            if (info.coordinates) {
                nlohmann::json coords;
                coords["latitude"]  = info.coordinates->latitude;
                coords["longitude"] = info.coordinates->longitude;
                coords["altitude"]  = info.coordinates->altitude;
                coords["accuracy"]  = info.coordinates->accuracy;
                coords["has_approximate_accuracy"] = info.coordinates->hasApproximateAccuracy;
                setValue("coordinates", coords);
            }
            if (info.locale) {
                nlohmann::json locale;
                locale["country_code"] = info.locale->countryCode;
                locale["city"]         = info.locale->city;
                locale["timezone"]     = info.locale->timezone;
                locale["language"]     = info.locale->language;
                setValue("locale", locale);
            }
        }

        const LocationContextProvider::LocationInfo& LocationContextProvider::getLocationInfo() const {
            return _locationInfo;
        }
    }
}
