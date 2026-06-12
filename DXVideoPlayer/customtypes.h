#pragma once
#include <string>
#include <vector>
#include <map>

class VideoSource;
class IRenderer;
class NetworkManager;
class Sequence;

struct AppState
{
    std::vector<VideoSource*> sources;
	std::map<std::string, VideoSource*> sourcesMap;
	std::map<int, std::string> choresMap;
	std::vector<Sequence*> sequences;
    IRenderer* renderer = nullptr;
    NetworkManager* networkMgr = nullptr;
	float lastSentPosition = 0.0f;
 };

struct TargetPosition {
    float position;
    std::string foreground;
    std::string background;
    float fade_in_seconds = 0.0f;
};

struct Config
{
    std::string choreos_config_file = "../config/choreos/config.json";
    std::string assets_path = "./assets";
    std::string target_ip = "127.0.0.1";
    std::string cover_filename = "Cover.mp4";
    int target_port = 15555;
    int control_port = 12345;
    double positions_framerate = 60.0;
    int send_period_ms = 40;
    int render_delay_ms = 100;
    int positions_delay_ms = 100;
    int autorun_id = -1;
    double positions_scale = 1.0;
    double positions_offset = 0.0;
    std::string autorun_filename = "";
    float cover_reference_speed = 200.0f;
    float cover_stop_acceleration = 200.0f;
    float cover_fade_in_time = 0.0f;
    float cover_fade_out_time = 0.0f;
    bool  disable_cover = false;
};

struct BackgroundEvent
{
    std::string filename;
    float startTime = 0.0f;
    float fadeInDuration = 0.0f;
    float fadeOutDuration = 0.0f;
    float duration = 0.0f;
    bool triggered = false;
};

struct VideoContent
{
    int id = -1;
    std::string name;
    //Path to the .mp4 file
    std::string filename;
    //Fade in duration in seconds (default 2.5s)
    float fadeInDuration = 2.5f;
    //Fade out duration in seconds (default 1s)
    float fadeOutDuration = 1.0f;
    //Whether the video should loop (default false)
    bool looped = false;
    //Optional position data loaded from a corresponding .csv file
    std::vector<float> positions;
    std::vector<BackgroundEvent> events;
};

enum class VideoTrackState
{
    Stopped,
	FadingIn,
    Playing,
    FadingOut   
};  

//Enumeration for different types of network commands that can be deferred and processed in the main thread
enum class NetworkCommandType
{
    Stop,
    PlayBackground,
    PlayForeground,
    PlaySequence,
    PlayCover,
    HideCover,
    TransitionTo,
	PlayChoreography
};

//Enumeration for different video layer types, which can be used to determine which track (foreground or cover) a command should target
enum class LayerType
{
    Background,
    Foreground,
    Cover
};

//Struct to represent a deferred network command, containing the command type and any associated data (e.g., filename for PlayForeground)
struct DeferredCommand
{
    NetworkCommandType type;
	int choreoID = -1;
    std::string filename;
    std::string foreground;
    std::string background;
    float fadeInDuration = 0.0f;
    float fadeOutDuration = 0.0f;
	float fgFadeOutDuration = 0.0f;
    bool looped = false;
	bool forceCoverOnExit = false;
};

//Struct to represent an item in a video sequence, containing the filename and fade durations
struct SequenceItem
{
    std::string filename;
    float fadeInDuration = 0.0f;
    float fadeOutDuration = 0.0f;
	bool looped = false;
};