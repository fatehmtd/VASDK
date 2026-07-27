#ifndef LOCATIONCONTEXTPROVIDER_H
#define LOCATIONCONTEXTPROVIDER_H

#include "ContextProvider.h"
#include <optional>

namespace va {
    namespace context {
        class LocationContextProvider : public ContextProvider {
            public:
            LocationContextProvider() = default;
            LocationContextProvider(const LocationContextProvider&) = delete;
            LocationContextProvider& operator=(const LocationContextProvider&) = delete;
            ~LocationContextProvider() override = default;

            struct Coordinates {
                double latitude;
                double longitude;
                double altitude;
                float  accuracy; // metres
                bool  hasApproximateAccuracy;
            };

            struct LocaleInfo {
                std::string countryCode; // ISO 3166-1 alpha-2 e.g. "US"
                std::string city;
                std::string timezone;   // IANA e.g. "America/New_York"
                std::string language;   // BCP 47 e.g. "en-US"
            };

            struct LocationInfo {
                std::optional<Coordinates> coordinates = std::nullopt;
                std::optional<LocaleInfo>  locale      = std::nullopt;
            };

            virtual void setLocationInfo(const LocationInfo& info);
            virtual const LocationInfo& getLocationInfo() const;

            private:
            LocationInfo _locationInfo;
        };
    }
}

#endif
