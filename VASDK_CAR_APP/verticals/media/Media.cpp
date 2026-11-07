#include "Media.h"

#include "Car.h"

#include <algorithm>

namespace car {

Media::Media(Car& car) : Vertical(car) {
    // shortcut: a fixed library; a head unit lists the phone's or the USB stick's music here.
    _library = {
        {"Blue Hour Avenue", "The Marlowe Quartet", "jazz", 252},
        {"Smoke and Brass", "Nina Calloway Trio", "jazz", 231},
        {"Gravel Road Anthem", "Iron Orchard", "rock", 214},
        {"Northbound", "The Static Pines", "rock", 198},
        {"Neon Summer", "Lila Monroe", "pop", 187},
        {"Paper Planes Tonight", "Juno Vale", "pop", 203},
        {"Nocturne in Rain", "Aurora Chamber Orchestra", "classical", 344},
        {"Morning Etude", "Elias Brandt", "classical", 276},
        {"Late Train Home", "Moss and Ember", "chill", 221},
        {"Coffee at Dusk", "Quiet Harbor", "chill", 239},
    };
    _stations = {
        {"coastal_fm", "Coastal FM", "101.5", "pop"},
        {"metro_news", "Metro News", "94.1", "news"},
        {"highway_rock", "Highway Rock", "104.7", "rock"},
        {"velvet_jazz", "Velvet Jazz", "88.3", "jazz"},
    };
    car.on("call.started", [this](const nlohmann::json&) { onCall(true); });
    car.on("call.ended", [this](const nlohmann::json&) { onCall(false); });
}

void Media::onCall(bool started) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (started && !_onCall) {
            _resumeAfterCall = _playing;
            _playing = false;
        } else if (!started && _onCall) {
            _playing = _playing || _resumeAfterCall;
            _resumeAfterCall = false;
        }
        _onCall = started;
    }
    changed();
}

nlohmann::json Media::stateLocked() const {
    nlohmann::json media{{"playing", _playing}, {"source", _source}, {"volume", _volume}, {"muted", _muted},
                         {"max_volume", _maxVolume}, {"video", _video}, {"on_call", _onCall}};
    if (_source == "radio") {
        const auto& station = _stations[_station];
        media["station"] = station.id;
        media["station_name"] = station.name;
        media["frequency"] = station.frequency;
        media["genre"] = station.genre;
        media["now_playing"] = station.name + " on " + station.frequency;
    } else {
        const auto& track = _library[_track];
        media["title"] = track.title;
        media["artist"] = track.artist;
        media["genre"] = track.genre;
        media["position_s"] = static_cast<int>(_position);
        media["duration_s"] = track.seconds;
        media["now_playing"] = track.title + " by " + track.artist;
    }
    media["source_name"] = _source == "radio" ? "the radio" : _source == "usb" ? "USB" : "Bluetooth";
    return media;
}

nlohmann::json Media::state() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return stateLocked();
}

bool Media::pickTrack(const std::function<bool(const Track&)>& fits) {
    // The next one that fits after the current track, so asking again plays another.
    for (size_t step = 1; step <= _library.size(); ++step) {
        const size_t index = (_track + step) % _library.size();
        if (!fits(_library[index])) continue;
        _track = index;
        _position = 0.0;
        if (_source == "radio") _source = "usb";
        _playing = true;
        return true;
    }
    return false;
}

nlohmann::json Media::command(const nlohmann::json& request) {
    nlohmann::json value;
    auto action = actionOf(request, value, {"genre", "artist", "station", "source", "volume"});   // {"next": true} too
    // An LLM may name what to play as its own argument: {"action": "play", "genre": "chill"}.
    for (const char* key : {"genre", "artist", "station", "source", "volume"}) {
        if (!request.contains(key) || request[key].is_null()) continue;
        if (action.empty() || action == "play" || action == key) {
            action = key;
            value = request[key];
            break;
        }
    }
    nlohmann::json result;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        bool changedNow = true;
        const bool plays = action == "play" || action == "next" || action == "previous" || action == "genre" ||
                           action == "artist" || action == "station" || action == "source";
        if (_onCall && plays) return error("the music waits until the call ends");
        if (action == "play") {
            changedNow = !_playing || _muted;
            _playing = true;
            _muted = false;
        } else if (action == "pause") {
            changedNow = _playing;
            _playing = false;
        } else if (action == "next" || action == "previous") {
            const bool next = action == "next";
            if (_source == "radio") {
                _station = (_station + (next ? 1 : _stations.size() - 1)) % _stations.size();
            } else if (!next && _position > 5.0) {
                _position = 0.0;   // as players do: back to the start of this one first
            } else {
                _track = (_track + (next ? 1 : _library.size() - 1)) % _library.size();
                _position = 0.0;
            }
            _playing = true;
        } else if (action == "volume" || action == "adjust_volume") {
            const int target = action == "volume" ? static_cast<int>(number(value, _volume)) : _volume + static_cast<int>(number(value, 3.0));
            const int volume = std::clamp(target, 0, _maxVolume);
            changedNow = volume != _volume || _muted;
            _volume = volume;
            _muted = false;
        } else if (action == "mute") {
            const bool on = truthy(value, true);
            changedNow = on != _muted;
            _muted = on;
        } else if (action == "source") {
            const auto source = lower(text(value));
            if (source != "radio" && source != "bluetooth" && source != "usb") return error("sources are radio, bluetooth and usb");
            changedNow = source != _source;
            _source = source;
            _position = 0.0;
            _playing = true;
        } else if (action == "station") {
            const auto id = lower(text(value));
            const auto found = std::find_if(_stations.begin(), _stations.end(), [&](const Station& s) { return s.id == id; });
            if (found == _stations.end()) return error("no station " + id);
            const auto index = static_cast<size_t>(found - _stations.begin());
            changedNow = _source != "radio" || index != _station;
            _source = "radio";
            _station = index;
            _playing = true;
        } else if (action == "genre") {
            const auto genre = lower(text(value));
            if (!pickTrack([&](const Track& t) { return t.genre == genre; })) return error("no " + genre + " music");
        } else if (action == "artist") {
            const auto artist = lower(text(value));
            if (!pickTrack([&](const Track& t) { return lower(t.artist) == artist; })) return error("nothing by " + artist);
        } else if (action == "video") {
            const bool on = truthy(value, true);
            changedNow = on != _video;
            _video = on;
            if (on) _playing = false;
        } else if (action == "status" || action.empty()) {
            changedNow = false;
        } else {
            return error("unknown media action: " + action);
        }
        result = stateLocked();
        result["changed"] = changedNow;
    }
    if (result.value("changed", false)) changed();
    return result;
}

std::vector<va::llm::Tool> Media::tools() {
    va::llm::Tool media("media", "Controls the car's music, radio and video");
    media.addParameter({"action", "play, pause, next, previous, volume (value 0-30), adjust_volume (value +3/-3), mute (on/off), "
                                  "source (radio, bluetooth or usb), station (coastal_fm, metro_news, highway_rock, velvet_jazz), "
                                  "genre (jazz, rock, pop, classical, chill), artist (a name), video (on/off, parked only), status",
                         "string", true});
    media.addParameter({"value", "the action's value: {\"action\": \"genre\", \"value\": \"chill\"}", "string", false});
    media.onCall([this](const nlohmann::json& args) { return command(args); });
    return {media};
}

void Media::tick(double seconds) {
    bool moved = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_playing && _source != "radio") {
            const double before = _position;
            _position += seconds;
            if (_position >= _library[_track].seconds) {
                _track = (_track + 1) % _library.size();
                _position = 0.0;
            }
            moved = static_cast<int>(before) / 5 != static_cast<int>(_position) / 5;   // the screen's progress bar, every 5 s
        }
    }
    if (moved) changed();
}

} // namespace car
