#pragma once
#include <vector>
#include <string>
#include "customtypes.h"

class PlaybackManager;

class Sequence
{
private:
	PlaybackManager* playbackMgr;

public:
	std::string name;
	std::vector<SequenceItem> items;
	int currentIndex = 0;
	bool isActive = false;
	bool looped = false;
	bool isFirstRun = true;

public:
	Sequence(std::string& name, const std::vector<SequenceItem>& items, PlaybackManager* playbackMgr);
	~Sequence() = default;

	void Play(bool shouldLoop = false);
	void Stop();
	void AdvanceSequence();
};

