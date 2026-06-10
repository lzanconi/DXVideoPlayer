#pragma once
#include <vector>
#include <map>
#include "customtypes.h"

class IApp;
class PlaybackManager;

class ContentManager
{
private:
	IApp* appInterface;
	std::vector<VideoContent> videoContents;
	std::map<std::string, VideoContent> videoContentsMap;

public:
	ContentManager(IApp* appInterface);
	~ContentManager() = default;

	// Scans the folder for .mp4 files and matching .csv position files
	void LoadContentsFromFolder(const std::string& folderPath);	
	void LoadContents(const std::string& folderPath);
	
	void LoadVideoContents(const std::string& folderPath);

	void LoadSequences(const std::string& folderPath, PlaybackManager* playbackMgr);

	// Returns the list of loaded video content
	const std::vector<VideoContent>& GetVideoContents() const;

private:
	void LoadVideoContentsFromConfig();
	void LoadVideoContentsFromFolder();
	void LoadCSVPositions(VideoContent& content, const std::string& csvPath);
	void LoadBackgroundEvents(VideoContent& content, const std::string& filePath);
	void ParseSequenceFile(const std::string& filePath, std::vector<SequenceItem>& outItems);
};

