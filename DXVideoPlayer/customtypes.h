#pragma once
#include <string>
#include <vector>

class VideoSource;
class IRenderer;
class NetworkManager;
class Sequence;

struct AppState
{
    std::vector<VideoSource*> sources;
	std::vector<Sequence*> sequences;
    IRenderer* renderer = nullptr;
    NetworkManager* networkMgr = nullptr;
 };

struct VideoContent
{
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
    PlayForeground,
    PlaySequence,
    PlayCover
};

//Enumeration for different video layer types, which can be used to determine which track (foreground or cover) a command should target
enum class LayerType
{
    Foreground,
    Cover
};

//Struct to represent a deferred network command, containing the command type and any associated data (e.g., filename for PlayForeground)
struct DeferredCommand
{
    NetworkCommandType type;
    std::string filename;
    float fadeInDuration = 0.0f;
    float fadeOutDuration = 0.0f;
    bool looped = false;
};

//Struct to represent an item in a video sequence, containing the filename and fade durations
struct SequenceItem
{
    std::string filename;
    float fadeInDuration = 1.0f;
    float fadeOutDuration = 1.0f;
	bool looped = false;
};