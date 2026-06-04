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