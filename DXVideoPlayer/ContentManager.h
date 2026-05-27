#pragma once
#include <vector>
#include "customtypes.h"

class IApp;

class ContentManager
{
private:
	IApp* appInterface;
	std::vector<VideoContent> videoContents;

public:
	ContentManager(IApp* appInterface);
	~ContentManager() = default;

	// Scans the folder for .mp4 files and matching .csv position files
	void LoadContentsFromFolder(const std::string& folderPath);	

	// Returns the list of loaded video content
	const std::vector<VideoContent>& GetVideoContents() const;

private:
	void LoadVideoContents(const std::string& folderPath);
	// Internal helper to parse position data from CSV files
	void LoadCSVPositions(VideoContent& content, const std::string& csvPath);
	void LoadSequences(const std::string& folderPath);
	void ParseSequenceFile(const std::string& filePath, std::vector<SequenceItem>& outItems);
};

