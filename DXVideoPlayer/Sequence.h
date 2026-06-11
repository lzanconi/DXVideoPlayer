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

public:
	Sequence(std::string& name, const std::vector<SequenceItem>& items, PlaybackManager* playbackMgr);
	~Sequence() = default;

	void Play(float overrideFadeIn = -1.0f);
	void Stop();
	void AdvanceSequence();
};

