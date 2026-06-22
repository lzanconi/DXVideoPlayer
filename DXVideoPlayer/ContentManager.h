#pragma once
#include <vector>
#include <map>
#include "customtypes.h"

class IApp;
class PlaybackManager;
struct ID3D11Device; 
struct ID3D11DeviceContext;

class ContentManager
{
private:
	IApp* appInterface;
	std::map<std::string, VideoContent> videoContentsMap;

public:
	ContentManager(IApp* appInterface);
	~ContentManager() = default;

	// Scans the folder for .mp4 files and matching .csv position files
	void LoadContents();
	
	void LoadSequences(const std::string& folderPath, PlaybackManager* playbackMgr);
	void LoadVideoSources(ID3D11Device* device, ID3D11DeviceContext* context);

	const std::map<std::string, VideoContent>& GetVideoContentsMap() const;

private:
	void LoadVideoContentsFromConfig();
	void LoadVideoContentsFromFolder();
	void LoadCSVPositions(VideoContent& content, const std::string& csvPath);
	void LoadBackgroundEvents(VideoContent& content, const std::string& filePath);
	void ParseSequenceFile(const std::string& filePath, std::vector<SequenceItem>& outItems);
};

