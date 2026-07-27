#pragma once
#include "bridge_tool.h"

//  get_volume 
class GetVolumeTool : public BridgeTool {
public:
    explicit GetVolumeTool(AndroidToolBridge bridge)
        : BridgeTool("get_volume",
                     "Get the current media volume and the maximum level. Takes no arguments - no level needed",
                     std::move(bridge)) {}
};

//  set_volume 
class SetVolumeTool : public BridgeTool {
public:
    explicit SetVolumeTool(AndroidToolBridge bridge)
        : BridgeTool("set_volume",
                     "Set the media volume. Only when the user explicitly asks to change the volume",
                     std::move(bridge)) {
        addParameter({"level", "Target volume level (integer, 0 = silent, max from get_volume)", "integer", true});
    }
};

//  media_play 
class MediaPlayTool : public BridgeTool {
public:
    explicit MediaPlayTool(AndroidToolBridge bridge)
        : BridgeTool("media_play",
                     "Resume music playback. Only when the user explicitly asks to play or resume music",
                     std::move(bridge)) {}
};

//  media_pause 
class MediaPauseTool : public BridgeTool {
public:
    explicit MediaPauseTool(AndroidToolBridge bridge)
        : BridgeTool("media_pause",
                     "Pause music playback. Only when the user explicitly asks to pause or stop music",
                     std::move(bridge)) {}
};

//  media_play_pause 
class MediaPlayPauseTool : public BridgeTool {
public:
    explicit MediaPlayPauseTool(AndroidToolBridge bridge)
        : BridgeTool("media_play_pause",
                     "Toggle music play/pause. Only when the user explicitly asks",
                     std::move(bridge)) {}
};

//  media_next_track 
class MediaNextTrackTool : public BridgeTool {
public:
    explicit MediaNextTrackTool(AndroidToolBridge bridge)
        : BridgeTool("media_next_track",
                     "Skip to the next track. Only when the user explicitly asks",
                     std::move(bridge)) {}
};

//  media_prev_track 
class MediaPrevTrackTool : public BridgeTool {
public:
    explicit MediaPrevTrackTool(AndroidToolBridge bridge)
        : BridgeTool("media_prev_track",
                     "Go back to the previous track. Only when the user explicitly asks",
                     std::move(bridge)) {}
};

//  get_now_playing 
class GetNowPlayingTool : public BridgeTool {
public:
    explicit GetNowPlayingTool(AndroidToolBridge bridge)
        : BridgeTool("get_now_playing",
                     "Get the currently playing track: title, artist, album, and playback state. Takes no arguments",
                     std::move(bridge)) {}
};
