#pragma once
#include <vector>
#include <string>
#include "customtypes.h"

class IApp;

class Sequence
{
private:
	IApp* appInterface;
	std::vector<SequenceItem> items;

public:
	std::string name;
	int currentIndex = 0;
	bool isActive = false;
	bool looped = false;
	bool isFirstRun = true;

public:
	Sequence(std::string& name, const std::vector<SequenceItem>& items, IApp* appInterface);
	~Sequence() = default;

	void Play(bool shouldLoop = false);
	void Stop();
	void AdvanceSequence();
};

