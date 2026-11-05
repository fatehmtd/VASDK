#pragma once

#include "Vertical.h"

#include <functional>
#include <mutex>

namespace car {

/**
 * Media: the radio, Bluetooth and USB, a library to play from by genre or artist, the volume, and
 * a video player for when the car is parked. Its tool, `media`, takes {"action", "value"}: play,
 * pause, next, previous, volume, adjust_volume, mute, source, station, genre, artist, video, status.
 * It pauses itself while the phone is in use (the "call.started" and "call.ended" signals).
 */
class Media : public Vertical {
public:
    explicit Media(Car& car);
    std::string name() const override { return "media"; }
    nlohmann::json state() const override;
    std::vector<va::llm::Tool> tools() override;
    void tick(double seconds) override;

    nlohmann::json command(const nlohmann::json& request);

    struct Track { std::string title, artist, genre; int seconds = 0; };
    struct Station { std::string id, name, frequency, genre; };

private:
    nlohmann::json stateLocked() const;
    bool pickTrack(const std::function<bool(const Track&)>& fits);
    void onCall(bool started);

    mutable std::mutex _mutex;
    std::vector<Track> _library;
    std::vector<Station> _stations;
    bool _playing = true;
    std::string _source = "radio";   // radio, bluetooth, usb
    size_t _station = 0;
    size_t _track = 0;
    double _position = 0.0;          // seconds into the track
    int _volume = 12;                // 0 to 30
    bool _muted = false;
    bool _video = false;             // the screen shows a video (parked only: the agent checks)
    bool _onCall = false;
    bool _resumeAfterCall = false;
    const int _maxVolume = 30;
};

} // namespace car
