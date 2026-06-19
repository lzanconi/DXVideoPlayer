#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>
#include "customtypes.h"
#include "VideoSource.h"

// Utility function to convert std::string (UTF-8) to std::wstring (UTF-16)
inline std::wstring stringToWS(const std::string& s) 
{
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::vector<wchar_t> buf(len);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf.data(), len);
    return std::wstring(buf.begin(), buf.end());
}

// Utility function to get the elapsed time in seconds since the first call to this function, using std::chrono for high-resolution timing.
inline double GetTimeStd()
{
    static auto start = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double>(now - start).count();
}

inline std::string VideoTrackStateToStr(VideoTrackState state)
{
    switch (state)
    {
        case VideoTrackState::Stopped: return "Stopped";
        case VideoTrackState::FadingIn: return "FadingIn";
        case VideoTrackState::Playing: return "Playing";
        case VideoTrackState::FadingOut: return "FadingOut";
        default: return "Unknown";
    }
}

inline std::string LayerTypeToStr(LayerType layerType)
{
    switch (layerType)
    {
        case LayerType::Foreground: return "Foreground";
        case LayerType::Cover: return "Cover";
        default: return "Unknown";
	}
}

inline std::string GetDurationMinSec(int totalSeconds)
{
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;

    std::stringstream ss;

    // std::setfill('0') sets the padding character to '0'
    // std::setw(2) forces the next number to take up at least 2 character spaces
    ss << std::setfill('0') << std::setw(2) << minutes << "m:"
        << std::setfill('0') << std::setw(2) << seconds << "s";

    return ss.str();
}

inline std::string GetFilenameFromPath(const std::string& path)
{
    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash == std::string::npos)
        return path;
    return path.substr(lastSlash + 1);
}

inline int FindVideoSourceIndexByFilename(const std::string& filename, const std::vector<VideoSource*>& sources)
{
    int matchIdx = -1;
    for (size_t i = 0; i < sources.size(); ++i)
    {
        if (sources[i]->filename == filename)
        {
            matchIdx = static_cast<int>(i);
            break;
        }
    }

	return matchIdx;
}

inline void TrimString(std::string& s)
{
    s.erase(0, s.find_first_not_of(" \t"));
    auto pos = s.find_last_not_of(" \t");
    if (pos != std::string::npos) 
    {
        s.erase(pos + 1);
    }
    else 
    {
        s.clear();
    }
}

inline void SetEnvVar(const std::string& key, const std::string& value)
{
    _putenv_s(key.c_str(), value.c_str());
}

inline std::string GetEnvVar(const std::string& key)
{
    char* buf = nullptr;
    size_t sz = 0;

    // Windows secure version
    if (_dupenv_s(&buf, &sz, key.c_str()) == 0 && buf != nullptr) {
        std::string result(buf);
        free(buf); // _dupenv_s requires manual freeing of the allocated buffer
        return result;
    }
    return "";
}   

// Utility function to extract the filename without extension from a given path.
inline std::string GetNameFromFile(const std::string& filename) {
    // Find the position of the last dot
    size_t lastDot = filename.find_last_of(".");

    // If no dot is found, return the whole string
    if (lastDot == std::string::npos) {
        return filename;
    }

    // Return the substring from the beginning to the dot
    return filename.substr(0, lastDot);
}

//Figures out which way the monitor is moving 
//Returns 1 (forward), -1 (backward) or 0 (stationary)
template <typename T>
inline int GetMovementDirection(T val) {
    return (T(0) < val) - (val < T(0));
}

// Smoothstep function for smooth interpolation, often used for easing in/out transitions. It takes a time value between 0 and 1 and returns a smoothed value that starts at 0, ends at 1, and has a smooth curve in between.
inline float smoothStep(double time)
{
    return time * time * (3.0 - 2.0 * time);
}

inline std::string GetTimestampStr()
{
    auto now = std::chrono::system_clock::now();
    std::time_t time_now = std::chrono::system_clock::to_time_t(now);

    std::tm local_time;
    localtime_s(&local_time, &time_now);

    std::stringstream ss;
    ss << std::put_time(&local_time, "%m/%d/%Y %H:%M:%S");

	return ss.str();
}