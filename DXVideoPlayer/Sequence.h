#pragma once
#include <vector>
#include <string>
#include "customtypes.h"

class IApp;

class Sequence
{
private:
	IApp* appInterface;

public:
	std::string name;
	std::vector<SequenceItem> items;
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

